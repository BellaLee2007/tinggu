#pragma once
// Include after EventBuffer/MpuTimedSample declarations.
// Full variable-length waveform features from the producer's original script.
struct TreeEventView : TingguWaveform14::Waveform {
  const EventBuffer &event;
  double baseline;
  TreeEventView(const EventBuffer &e, double b):event(e),baseline(b){}
  unsigned count()const override{return event.piezoCount;}
  double timeMs(unsigned i)const override{return (double(i)-event.triggerIndex)*0.5;}
  double delta(unsigned i)const override{return double(event.piezo[i])-baseline;}
  TingguWaveform14::MpuMoments mpu()const override {
    TingguWaveform14::MpuMoments m;int lastRow=-1;
    for(unsigned i=0;i<event.mpuCount;++i){const MpuTimedSample &v=event.mpu[i];
      int row=(int)lround((int32_t)(v.timestampUs-event.eventStartTimestampUs)/500.0);
      if(row<0 || row>=(int)event.piezoCount || row==lastRow)continue;lastRow=row;
      // Match units/CSV precision used by the original training exporter.
      double ax=round(v.ax/2048.0*1e7)/1e7,ay=round(v.ay/2048.0*1e7)/1e7,az=round(v.az/2048.0*1e7)/1e7;
      double gx=round(v.gx/32.8*1e6)/1e6,gy=round(v.gy/32.8*1e6)/1e6,gz=round(v.gz/32.8*1e6)/1e6;
      ++m.validRows;m.acceleration.add(sqrt(ax*ax+ay*ay+az*az));m.gyroscope.add(sqrt(gx*gx+gy*gy+gz*gz));
    }
    return m;
  }
};
inline bool extractWaveformModelFeatures(const EventBuffer &event,TingguWaveform14::Workspace &treeWorkspace,double *features) {
  if(event.sampleRateHz!=2000 || !event.triggerIndex || event.triggerIndex>=event.piezoCount)return false;
  for(unsigned i=0;i<event.triggerIndex;++i)treeWorkspace.real[i]=event.piezo[i];
  std::sort(treeWorkspace.real,treeWorkspace.real+event.triggerIndex);
  unsigned n=event.triggerIndex;
  double baseline=n%2?treeWorkspace.real[n/2]:(treeWorkspace.real[n/2-1]+treeWorkspace.real[n/2])/2;
  TreeEventView view(event,baseline);
  return TingguWaveform14::extract(view,treeWorkspace,features);
}

