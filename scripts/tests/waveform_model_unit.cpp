#include <cstdint>
#include <cassert>
#include <limits>
#include <iostream>
#include "model_interface.h"
#include "model_selftest.h"
static TingguWaveform14::Workspace w;
int main(){assert(tingguModelStartupSelfTest(w));double x[14]={};x[2]=std::numeric_limits<double>::quiet_NaN();assert(inferTingguModel(x).classification==TINGGU_MODEL_UNTRAINED);assert(tingguTreeLeaf(nullptr)<0);
 TingguModelOutput votes[3]={};for(int i=0;i<3;++i){votes[i].classification=static_cast<TingguModelClass>(i);votes[i].probabilities[i]=1;votes[i].confidence=1;}
 assert(averageTingguModelOutputs(votes,3).classification==TINGGU_MODEL_UNCERTAIN);
 votes[2]=votes[0];assert(averageTingguModelOutputs(votes,3).classification==TINGGU_TIGHT);
 votes[2]=emptyTingguModelOutput();assert(averageTingguModelOutputs(votes,3).classification==TINGGU_MODEL_UNTRAINED);
 std::cout<<"PASS startup raw-waveform golden, invalid input, 3-strike consensus/tie/invalid\n";
}
