using System;
using System.Collections.Generic;

namespace TingguSignalWorkbench
{
    internal sealed class SampleData
    {
        public int Index;
        public int TimeUs;
        public ushort PiezoRaw;
        public bool MpuValid;
        public short Ax;
        public short Ay;
        public short Az;
        public short Gx;
        public short Gy;
        public short Gz;
    }

    internal sealed class EventData
    {
        public int EventId;
        public int ExpectedSamples;
        public int PreSamples;
        public int PostSamples;
        public int PiezoRate;
        public int MpuRate;
        public double FirmwarePiezoBaseline;
        public int TriggerThreshold;
        public bool MpuAvailable;
        public double AccelLsbPerG;
        public double GyroLsbPerDps;
        public int FirmwareMissedTicks;
        public int FirmwareMpuFailures;
        public uint DeclaredChecksum;
        public uint CalculatedChecksum;
        public bool IntegrityValid;
        public string IntegrityMessage;
        public readonly List<SampleData> Samples = new List<SampleData>();
    }

    internal sealed class ExperimentMetadata
    {
        public string SpecimenId = "";
        public string TightnessLabel = "";
        public string Torque = "";
        public string StrikeMethod = "手敲";
        public string Notes = "";

        public string DisplayLabel
        {
            get
            {
                if (!String.IsNullOrWhiteSpace(SpecimenId) && !String.IsNullOrWhiteSpace(TightnessLabel))
                    return SpecimenId.Trim() + "_" + TightnessLabel.Trim();
                if (!String.IsNullOrWhiteSpace(TightnessLabel)) return TightnessLabel.Trim();
                if (!String.IsNullOrWhiteSpace(SpecimenId)) return SpecimenId.Trim();
                return "unlabeled";
            }
        }
    }

    internal sealed class AnalysisResult
    {
        public double PiezoBaseline;
        public double PiezoNoiseRms;
        public double PiezoPositivePeak;
        public double PiezoNegativePeak;
        public double PiezoAbsolutePeak;
        public double PiezoPeakToPeak;
        public double PiezoSnrDb;
        public double PiezoDurationMs;
        public bool PiezoDurationTruncated;
        public bool PiezoSaturated;

        public double MpuNoiseRmsG;
        public double MpuPositivePeakG;
        public double MpuNegativePeakG;
        public double MpuAbsolutePeakG;
        public double MpuPeakToPeakG;
        public double MpuVectorPeakG;
        public double MpuSnrDb;
        public double MpuDurationMs;
        public bool MpuDurationTruncated;
        public bool MpuSaturated;
        public string DominantAxis = "N/A";
        public double DominantFrequencyHz;
        public double PeriodMs;

        public double DropRatePercent;
        public bool PiezoQuality;
        public bool MpuQuality;
        public bool DualSensorValid;
        public string QualityText = "";

        public double MpuMeanAxRaw;
        public double MpuMeanAyRaw;
        public double MpuMeanAzRaw;
        public double MpuMeanGxRaw;
        public double MpuMeanGyRaw;
        public double MpuMeanGzRaw;
    }

    internal sealed class CompletedEvent
    {
        public EventData Event;
        public AnalysisResult Analysis;
        public ExperimentMetadata Metadata;
        public string CsvPath;
    }
}
