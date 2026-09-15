"""Verify existing tree on original producer features and the shared C++ extractor. No fitting."""
from pathlib import Path
import argparse, json, subprocess, importlib.util
import numpy as np
import pandas as pd
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("tinggu_producer",ROOT/"models/20260913/train_waveform_14features.py")
producer=importlib.util.module_from_spec(spec)
spec.loader.exec_module(producer)
a=argparse.ArgumentParser();a.add_argument('--dataset',type=Path,default=ROOT/'data/training_dataset_20260911');a.add_argument('--output',type=Path,default=ROOT/'tmp/tree_verification');a.add_argument('--cxx',default='D:/mingw64/bin/g++.exe');args=a.parse_args();args.output.mkdir(parents=True,exist_ok=True)
model=json.loads((ROOT/'models/20260913/decision_tree_full.json').read_text());assert model['feature_names']==producer.FEATURE_NAMES
manifest=pd.read_csv(args.dataset/'manifest.csv');rows=[];counts={};matrix=np.zeros((3,3),int)
for _,row in manifest.iterrows():
 if row.state_label not in producer.LABELS:raise ValueError('Missing or unknown explicit label')
 path=(args.dataset/row.state_label/row.file).resolve();x=producer.extract_features(path);assert np.isfinite(x).all()
 n=model['root'];route='root';counts[route]=counts.get(route,0)+1
 while 'j' in n:
  left=x[n['j']]<=n['thr'];route=('' if route=='root' else route)+('L' if left else 'R');n=n['left' if left else 'right'];counts[route]=counts.get(route,0)+1
 matrix[producer.LABELS.index(row.state_label),n['pred']]+=1
 frame=pd.read_csv(path)
 rows.append(dict(file=str(path),label=row.state_label,measurement_id=row.measurement_id,predicted=n['pred'],regular=bool((np.diff(frame.time_us)==500).all()),**dict(zip(producer.FEATURE_NAMES,x))))
def check(n,route='root'):
 assert counts.get(route,0)==n['n'],(route,counts.get(route,0),n['n'])
 if 'j' in n:
  check(n['left'],('' if route=='root' else route)+'L');check(n['right'],('' if route=='root' else route)+'R')
check(model['root']);frame=pd.DataFrame(rows);frame.to_csv(args.output/'producer_features.csv',index=False)
report={'events':len(frame),'node_population_match':True,'replay_confusion':matrix.tolist(),'replay_accuracy_not_test_accuracy':float(np.trace(matrix)/matrix.sum()),'checks':{}}
for mode,source in [('features','waveform_feature_host.cpp'),('adapter','waveform_adapter_host.cpp'),('units','waveform_model_unit.cpp')]:
 exe=args.output/(mode+'.exe')
 subprocess.run([args.cxx,'-std=c++11','-O2','-I'+str(ROOT/'firmware/tinggu_edge_runtime'),str(Path(__file__).parent/'tests'/source),'-o',str(exe)],check=True)
 if mode=='units':
  result=subprocess.run([str(exe)],check=True,text=True,capture_output=True);report['checks'][mode]=result.stdout.strip();continue
 batch=frame if mode=='features' else frame[frame.regular]
 result=subprocess.run([str(exe)],input='\n'.join(batch.file)+'\n',text=True,capture_output=True,check=True)
 actual=np.array([[float(v) for v in line.split(',')] for line in result.stdout.splitlines()]);expected=batch[producer.FEATURE_NAMES].to_numpy()
 np.testing.assert_allclose(actual[:,:14],expected,rtol=1e-7,atol=2e-6)
 assert (actual[:,14]==batch.predicted.to_numpy()).all()
 report['checks'][mode]={'cases':len(batch),'prediction_mismatches':0,'max_abs_error':dict(zip(producer.FEATURE_NAMES,np.max(abs(actual[:,:14]-expected),axis=0).tolist()))}
(args.output/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report))
