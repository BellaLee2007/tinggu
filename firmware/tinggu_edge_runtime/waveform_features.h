#pragma once
#include <cmath>
#include <cstddef>
#include <algorithm>
// Exact feature contract: train_waveform_14features.py at 4859fa6.
// No heap allocation. One processing task owns this workspace.
namespace TingguWaveform14 {
static const unsigned FEATURE_COUNT = 14;
static const unsigned FFT_CAPACITY = 4096;
struct Moments {
  unsigned n = 0;
  double mean = 0, m2 = 0;
  void add(double x) { ++n; double d=x-mean; mean+=d/n; m2+=d*(x-mean); }
  double stddev() const { return n ? std::sqrt(std::max(0.0,m2/n)) : 0.0; }
};
struct MpuMoments { Moments acceleration, gyroscope; unsigned validRows=0; };
struct Waveform {
  virtual unsigned count() const = 0;
  virtual double timeMs(unsigned i) const = 0;
  virtual double delta(unsigned i) const = 0;
  virtual MpuMoments mpu() const = 0;
  virtual ~Waveform() {}
};
struct Workspace { double real[FFT_CAPACITY]; double imag[FFT_CAPACITY]; };
inline void fft(Workspace &w, unsigned n) {
  for(unsigned i=1,j=0;i<n;++i){ unsigned bit=n>>1; for(;j&bit;bit>>=1)j^=bit; j^=bit;
    if(i<j){std::swap(w.real[i],w.real[j]);std::swap(w.imag[i],w.imag[j]);}}
  const double pi=3.1415926535897932384626433832795;
  for(unsigned len=2;len<=n;len<<=1){double angle=-2*pi/len, ar=std::cos(angle),ai=std::sin(angle);
    for(unsigned i=0;i<n;i+=len){double wr=1,wi=0;
      for(unsigned j=0;j<len/2;++j){unsigned a=i+j,b=a+len/2;
        double vr=w.real[b]*wr-w.imag[b]*wi,vi=w.real[b]*wi+w.imag[b]*wr;
        double ur=w.real[a],ui=w.imag[a];w.real[a]=ur+vr;w.imag[a]=ui+vi;w.real[b]=ur-vr;w.imag[b]=ui-vi;
        double nr=wr*ar-wi*ai;wi=wr*ai+wi*ar;wr=nr;
      }
    }
  }
}
inline bool extract(const Waveform &v, Workspace &w, double out[FEATURE_COUNT]) {
  unsigned total=v.count(); if(total<16 || total>FFT_CAPACITY)return false;
  unsigned first=0; while(first<total && v.timeMs(first)<0)++first;
  if(total-first<16)first=0;
  unsigned n=total-first, nfft=256;while(nfft<n)nfft<<=1;
  if(nfft>FFT_CAPACITY)return false;
  Moments all,post; bool allFinite=true;unsigned peak=0;double peakAbs=-1;
  double earlyPower=0,latePower=0;unsigned earlyN=0,lateN=0;
  for(unsigned i=0;i<total;++i){double x=v.delta(i);if(!std::isfinite(x))allFinite=false;else all.add(x);}
  for(unsigned i=0;i<n;++i){double x=v.delta(first+i);if(std::isnan(x))x=0;if(!std::isfinite(x))return false;
    double t=v.timeMs(first+i);post.add(x);if(std::fabs(x)>peakAbs){peakAbs=std::fabs(x);peak=i;}
    if(t>=0&&t<100){earlyPower+=x*x;++earlyN;}if(t>=100&&t<300){latePower+=x*x;++lateN;}
  }
  const double pi=3.1415926535897932384626433832795;
  for(unsigned i=0;i<nfft;++i){double x=i<n?v.delta(first+i):0;if(std::isnan(x))x=0;
    w.real[i]=i<n?(x-post.mean)*(0.5-0.5*std::cos(2*pi*i/(n-1))):0;w.imag[i]=0;}
  fft(w,nfft);double best=0,dom=0,sum=0,weighted=0,low=0,high=0;
  for(unsigned k=0;k<=nfft/2;++k){double f=k*2000.0/nfft;if(f<5||f>1000)continue;
    double power=w.real[k]*w.real[k]+w.imag[k]*w.imag[k];double mag=std::sqrt(power);
    if(mag>best){best=mag;dom=f;}sum+=mag;weighted+=f*mag;if(f<100)low+=power;else high+=power;
  }
  double tau=9999;unsigned start=peak+4,end=std::min(n,start+500);
  if(end>start && end-start>=20){double mx=0;for(unsigned i=start;i<end;++i){double x=v.delta(first+i);if(std::isnan(x))x=0;mx=std::max(mx,std::fabs(x)+1e-6);}
    double threshold=std::max(mx*.02,1e-5);unsigned kept=0;double sx=0,sy=0,sxx=0,sxy=0;
    for(unsigned i=start;i<end;++i){double a=v.delta(first+i);if(std::isnan(a))a=0;double env=std::fabs(a)+1e-6;
      if(env>threshold){double x=(i-start)*.5,y=std::log(env);++kept;sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;}}
    if(kept>=10){double denominator=kept*sxx-sx*sx;double slope=denominator!=0?(kept*sxy-sx*sy)/denominator:0;if(slope < -1e-5)tau=-1/slope;}
  }
  MpuMoments m=v.mpu();
  out[0]=peakAbs;out[1]=v.timeMs(first+peak);out[2]=earlyN?std::sqrt(earlyPower/earlyN):0;out[3]=lateN?std::sqrt(latePower/lateN):0;
  out[4]=best>0?dom:0;out[5]=sum>0?weighted/sum:0;out[6]=best>0?low/(high+1e-12):0;out[7]=tau;out[8]=allFinite?all.stddev():0;
  out[9]=m.acceleration.mean;out[10]=m.acceleration.stddev();out[11]=m.gyroscope.mean;out[12]=m.gyroscope.stddev();out[13]=double(m.validRows)/total;
  for(unsigned i=0;i<FEATURE_COUNT;++i){if(std::isnan(out[i]))out[i]=0;else if(!std::isfinite(out[i]))out[i]=out[i]>0?9999:-9999;}
  return true;
}
}
