"""Train Tinggu's 14-feature waveform decision-tree baseline.

Example:
    python train_waveform_14features.py \
        --dataset /path/to/training_dataset_20260911 \
        --outdir ./model_output

The dataset directory must contain manifest.csv and tight/, medium/, loose/
subdirectories with the event CSV files.
"""

import argparse, csv, json, math, os, sys
from pathlib import Path
from collections import Counter, defaultdict
import numpy as np
import pandas as pd
from PIL import Image, ImageDraw, ImageFont

LABELS = ['tight', 'medium', 'loose']
FEATURE_NAMES = [
    'peak_abs', 'peak_time_ms', 'rms_0_100ms', 'rms_100_300ms',
    'dominant_freq_hz', 'spectral_centroid_hz', 'low_high_energy_ratio',
    'decay_tau_ms', 'piezo_std', 'accel_mag_mean', 'accel_mag_std',
    'gyro_mag_mean', 'gyro_mag_std', 'mpu_valid_ratio'
]

def safe_float(x):
    try:
        return float(x)
    except Exception:
        return np.nan

def extract_features(path):
    d = pd.read_csv(path)
    t = d['time_us'].to_numpy(dtype=float) / 1000.0
    y = d['piezo_delta'].to_numpy(dtype=float)
    y = np.nan_to_num(y, nan=0.0)
    post = t >= 0
    if post.sum() < 16:
        post = np.ones(len(t), dtype=bool)
    tp, yp = t[post], y[post]
    abs_y = np.abs(yp)
    peak_i = int(np.argmax(abs_y))
    peak = float(abs_y[peak_i])
    peak_t = float(tp[peak_i])
    def rms(a, b):
        z = yp[(tp >= a) & (tp < b)]
        return float(np.sqrt(np.mean(z*z))) if len(z) else 0.0
    # FFT on the post-trigger window, with a Hann window and zero padding.
    z = yp - np.mean(yp)
    n = len(z)
    nfft = 1 << max(8, int(math.ceil(math.log2(max(256, n)))))
    win = np.hanning(n)
    spec = np.abs(np.fft.rfft(z * win, n=nfft))
    freqs = np.fft.rfftfreq(nfft, d=0.0005)
    band = (freqs >= 5) & (freqs <= 1000)
    if band.sum() and np.max(spec[band]) > 0:
        sb = spec[band]; fb = freqs[band]
        di = int(np.argmax(sb)); dom = float(fb[di])
        centroid = float(np.sum(fb * sb) / np.sum(sb))
        low = float(np.sum(sb[(fb >= 5) & (fb < 100)] ** 2))
        high = float(np.sum(sb[(fb >= 100) & (fb <= 1000)] ** 2))
        ratio = low / (high + 1e-12)
    else:
        dom = centroid = ratio = 0.0
    # Exponential ring-down estimate from the post-peak envelope.
    start = peak_i + 4
    end = min(len(yp), start + 500)
    if end - start >= 20:
        env = np.abs(yp[start:end]) + 1e-6
        xx = np.arange(len(env), dtype=float) * 0.5
        keep = env > max(np.max(env) * 0.02, 1e-5)
        if keep.sum() >= 10:
            slope = np.polyfit(xx[keep], np.log(env[keep]), 1)[0]
            tau = float(-1.0 / slope) if slope < -1e-5 else 9999.0
        else: tau = 9999.0
    else: tau = 9999.0
    pv = d['piezo_delta'].to_numpy(dtype=float)
    mvalid = d['mpu_valid'].fillna(0).to_numpy(dtype=float) > 0
    ax = d['ax_g'].to_numpy(dtype=float); ay = d['ay_g'].to_numpy(dtype=float); az = d['az_g'].to_numpy(dtype=float)
    gx = d['gx_dps'].to_numpy(dtype=float); gy = d['gy_dps'].to_numpy(dtype=float); gz = d['gz_dps'].to_numpy(dtype=float)
    amag = np.sqrt(np.nan_to_num(ax)**2 + np.nan_to_num(ay)**2 + np.nan_to_num(az)**2)[mvalid]
    gmag = np.sqrt(np.nan_to_num(gx)**2 + np.nan_to_num(gy)**2 + np.nan_to_num(gz)**2)[mvalid]
    if not len(amag): amag = np.array([0.0])
    if not len(gmag): gmag = np.array([0.0])
    vals = [peak, peak_t, rms(0,100), rms(100,300), dom, centroid, ratio, tau,
            float(np.std(pv)), float(np.mean(amag)), float(np.std(amag)),
            float(np.mean(gmag)), float(np.std(gmag)), float(np.mean(mvalid))]
    return np.nan_to_num(np.asarray(vals, dtype=float), nan=0.0, posinf=9999.0, neginf=-9999.0)

class Tree:
    def __init__(self, max_depth=4, min_leaf=3): self.max_depth=max_depth; self.min_leaf=min_leaf; self.node=None
    def fit(self, X, y): self.node=self._grow(X,y,0); return self
    def _grow(self, X, y, depth):
        counts=np.bincount(y, minlength=3); pred=int(np.argmax(counts)); node={'pred':pred,'n':len(y)}
        if depth>=self.max_depth or len(y)<2*self.min_leaf or np.max(counts)==len(y): return node
        best=None; base=self._gini(y)
        for j in range(X.shape[1]):
            order=np.argsort(X[:,j]); xs=X[order,j]; ys=y[order]
            for k in range(self.min_leaf, len(y)-self.min_leaf+1):
                if k>=len(y) or xs[k]==xs[k-1]: continue
                left,right=ys[:k],ys[k:]
                gain=base-(len(left)*self._gini(left)+len(right)*self._gini(right))/len(y)
                if best is None or gain>best[0]: best=(gain,j,(xs[k]+xs[k-1])/2)
        if best is None or best[0] <= 1e-9: return node
        _,j,thr=best; mask=X[:,j] <= thr
        if mask.sum()<self.min_leaf or (~mask).sum()<self.min_leaf: return node
        node.update({'j':j,'thr':float(thr),'left':self._grow(X[mask],y[mask],depth+1),'right':self._grow(X[~mask],y[~mask],depth+1)})
        return node
    @staticmethod
    def _gini(y):
        if len(y)==0:return 0.0
        c=np.bincount(y,minlength=3)/len(y); return float(1-np.sum(c*c))
    def predict_one(self,x):
        n=self.node
        while 'j' in n: n=n['left'] if x[n['j']]<=n['thr'] else n['right']
        return n['pred']
    def predict(self,X): return np.asarray([self.predict_one(x) for x in X],dtype=int)

def make_folds(groups, labels, k=5):
    # Greedy stratification at the group level, deterministic by group id.
    by=defaultdict(list)
    for g,l in zip(groups,labels): by[g].append(l)
    fold_groups=[[] for _ in range(k)]; totals=[Counter() for _ in range(k)]
    items=sorted(by.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    for g,ls in items:
        scores=[(sum(totals[i][l] for l in LABELS), len(fold_groups[i]), i) for i in range(k)]
        i=min(scores)[2]; fold_groups[i].append(g); totals[i].update(ls)
    return [set(x) for x in fold_groups]

def metrics(y, p):
    cm=np.zeros((3,3),dtype=int)
    for a,b in zip(y,p): cm[a,b]+=1
    acc=float(np.trace(cm)/max(1,len(y))); per=[]
    for i in range(3):
        tp=cm[i,i]; prec=tp/max(1,cm[:,i].sum()); rec=tp/max(1,cm[i,:].sum()); f=2*prec*rec/max(1e-12,prec+rec)
        per.append({'precision':prec,'recall':rec,'f1':f,'support':int(cm[i,:].sum())})
    return cm,acc,per

def draw_cm(cm, path, title):
    W=760; im=Image.new('RGB',(W,620),'white'); dr=ImageDraw.Draw(im)
    try: font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',22); small=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',18); bold=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',24)
    except: font=small=bold=None
    # Keep long dataset names readable on a fixed-size image.
    if len(title) > 48:
        title = title.replace(' - 5-fold grouped OOF', '\n5-fold grouped OOF')
    dr.multiline_text((30,20),title,fill='black',font=bold,spacing=4)
    x0,y0=180,150; cell=120; mx=max(1,int(cm.max()))
    for i,l in enumerate(LABELS):
        dr.text((x0+i*cell+35,y0-42),l,fill='black',font=small); dr.text((x0-95,y0+i*cell+47),l,fill='black',font=small)
    for i in range(3):
        for j in range(3):
            v=int(cm[i,j]); frac=v/mx; c=(int(245-150*frac),int(248-130*frac),int(250-80*frac)); dr.rectangle((x0+j*cell,y0+i*cell,x0+(j+1)*cell,y0+(i+1)*cell),fill=c,outline=(120,120,120),width=2)
            s=str(v); box=dr.textbbox((0,0),s,font=font); dr.text((x0+j*cell+(cell-(box[2]-box[0]))/2,y0+i*cell+43),s,fill='black',font=font)
    dr.text((x0+90,y0+3*cell+25),'Predicted label',fill='black',font=small); dr.text((25,y0+120),'True label',fill='black',font=small)
    im.save(path)

def run_dataset(root, outdir):
    root=Path(root); outdir=Path(outdir); outdir.mkdir(parents=True,exist_ok=True)
    manifest=pd.read_csv(root/'manifest.csv')
    X=[]; y=[]; groups=[]; files=[]
    for _,r in manifest.iterrows():
        f=root / str(r['state_label']) / r['file']
        if not f.exists():
            f=root / r['file']
        X.append(extract_features(f)); y.append(LABELS.index(r['state_label'])); groups.append(str(r['measurement_id'])); files.append(str(f))
    X=np.asarray(X); y=np.asarray(y); groups=np.asarray(groups)
    folds=make_folds(groups,[LABELS[i] for i in y],5); oof=np.full(len(y),-1); fold_rows=[]
    for fi,testg in enumerate(folds):
        te=np.array([g in testg for g in groups]); tr=~te
        model=Tree(4,3).fit(X[tr],y[tr]); pred=model.predict(X[te]); oof[te]=pred
        cm,acc,per=metrics(y[te],pred); fold_rows.append({'fold':fi+1,'train_events':int(tr.sum()),'test_events':int(te.sum()),'test_groups':len(testg),'accuracy':acc,'macro_f1':float(np.mean([z['f1'] for z in per]))})
    cm,acc,per=metrics(y,oof)
    model=Tree(4,3).fit(X,y)
    # Feature importance surrogate: count split usage weighted by sample count.
    used=Counter()
    def walk(n):
        if 'j' in n: used[n['j']]+=n['n']; walk(n['left']); walk(n['right'])
    walk(model.node); total=max(1,sum(used.values())); fi=[{'feature':FEATURE_NAMES[i],'importance':used[i]/total} for i in range(len(FEATURE_NAMES)) if used[i]]
    fi.sort(key=lambda z:-z['importance'])
    result={'dataset':root.name,'events':int(len(y)),'groups':int(len(set(groups))),'class_counts':dict(Counter(LABELS[i] for i in y)),'features':FEATURE_NAMES,'tree':{'max_depth':4,'min_leaf':3},'cv':{'scheme':'5-fold GroupKFold-like stratified by measurement_id','folds':fold_rows,'oof_accuracy':acc,'oof_macro_f1':float(np.mean([z['f1'] for z in per])),'confusion_matrix':cm.tolist(),'per_class':{LABELS[i]:per[i] for i in range(3)}},'feature_importance_surrogate':fi}
    (outdir/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    (outdir/'tree_model.json').write_text(json.dumps({'feature_names':FEATURE_NAMES,'max_depth':4,'min_leaf':3,'root':model.node},ensure_ascii=False,indent=2),encoding='utf-8')
    feature_df = pd.DataFrame(X, columns=FEATURE_NAMES)
    feature_df.insert(0, 'file', files)
    feature_df.insert(1, 'measurement_id', groups)
    feature_df.insert(2, 'label', [LABELS[i] for i in y])
    feature_df.to_csv(outdir/'features_14.csv', index=False)
    pd.DataFrame(fold_rows).to_csv(outdir/'fold_metrics.csv',index=False)
    pd.DataFrame({'file':files,'measurement_id':groups,'true_label':[LABELS[i] for i in y],'predicted_label':[LABELS[i] for i in oof]}).to_csv(outdir/'oof_predictions.csv',index=False)
    draw_cm(cm,outdir/'confusion_matrix.png',root.name+' - 5-fold grouped OOF')
    return result

if __name__=='__main__':
    parser = argparse.ArgumentParser(description='Extract 14 physical features from Tinggu waveforms and train a grouped decision tree.')
    parser.add_argument('--dataset', required=True, help='Dataset directory containing manifest.csv and class subdirectories.')
    parser.add_argument('--outdir', required=True, help='Directory for features_14.csv, tree_model.json, metrics, predictions, and confusion_matrix.png.')
    args = parser.parse_args()
    result = run_dataset(Path(args.dataset), Path(args.outdir))
    print(json.dumps({
        'dataset': result['dataset'],
        'events': result['events'],
        'groups': result['groups'],
        'oof_accuracy': result['cv']['oof_accuracy'],
        'oof_macro_f1': result['cv']['oof_macro_f1'],
        'confusion_matrix': result['cv']['confusion_matrix'],
    }, ensure_ascii=False, indent=2))
