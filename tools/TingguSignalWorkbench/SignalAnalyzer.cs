using System;
using System.Collections.Generic;
using System.Linq;

namespace TingguSignalWorkbench
{
    internal static class SignalAnalyzer
    {
        public static AnalysisResult Analyze(EventData data, double piezoSnrMinimum, double mpuSnrMinimum)
        {
            if (data == null || data.Samples.Count == 0)
                throw new ArgumentException("事件中没有采样数据。", "data");

            AnalysisResult result = new AnalysisResult();
            AnalyzePiezo(data, result);
            AnalyzeMpu(data, result);
            result.DropRatePercent = CalculateDropRate(data) * 100.0;

            result.PiezoQuality = data.IntegrityValid && !result.PiezoSaturated &&
                                  IsFinite(result.PiezoSnrDb) && result.PiezoSnrDb >= piezoSnrMinimum;
            result.MpuQuality = data.IntegrityValid && data.MpuAvailable && !result.MpuSaturated &&
                                result.DropRatePercent < 1.0 && IsFinite(result.MpuSnrDb) &&
                                result.MpuSnrDb >= mpuSnrMinimum;
            result.DualSensorValid = result.PiezoQuality && result.MpuQuality;

            if (result.DualSensorValid)
                result.QualityText = "双传感器信号有效";
            else if (!data.IntegrityValid)
                result.QualityText = "无效：传输不完整（" + data.IntegrityMessage + "）";
            else if (!data.MpuAvailable)
                result.QualityText = result.PiezoQuality ? "仅压电有效：未找到MPU-6050" : "无效：压电质量不足且未找到MPU";
            else
                result.QualityText = "未通过：" + BuildFailureReason(result, piezoSnrMinimum, mpuSnrMinimum);

            return result;
        }

        private static void AnalyzePiezo(EventData data, AnalysisResult result)
        {
            List<double> pre = data.Samples.Where(delegate(SampleData s) { return s.TimeUs < 0; })
                                           .Select(delegate(SampleData s) { return (double)s.PiezoRaw; }).ToList();
            if (pre.Count == 0) pre.Add(data.FirmwarePiezoBaseline);
            result.PiezoBaseline = Mean(pre);
            List<double> preDelta = pre.Select(delegate(double v) { return v - result.PiezoBaseline; }).ToList();
            result.PiezoNoiseRms = Rms(preDelta);
            if (result.PiezoNoiseRms < 0.001) result.PiezoNoiseRms = 0.001;

            List<SampleData> postRows = data.Samples.Where(delegate(SampleData s) { return s.TimeUs >= 0; }).ToList();
            List<double> post = postRows.Select(delegate(SampleData s) { return s.PiezoRaw - result.PiezoBaseline; }).ToList();
            if (post.Count == 0) post.Add(0.0);

            result.PiezoPositivePeak = post.Max();
            result.PiezoNegativePeak = post.Min();
            result.PiezoAbsolutePeak = post.Max(delegate(double v) { return Math.Abs(v); });
            result.PiezoPeakToPeak = result.PiezoPositivePeak - result.PiezoNegativePeak;
            result.PiezoSaturated = data.Samples.Any(delegate(SampleData s) { return s.PiezoRaw <= 5 || s.PiezoRaw >= 4090; });

            double eventPower = MeanSquare(postRows.Where(delegate(SampleData s) { return s.TimeUs <= 300000; })
                .Select(delegate(SampleData s) { return s.PiezoRaw - result.PiezoBaseline; }));
            result.PiezoSnrDb = CalculateSnr(eventPower, result.PiezoNoiseRms * result.PiezoNoiseRms);

            DurationResult duration = FindDuration(
                postRows.Select(delegate(SampleData s) { return s.TimeUs / 1000.0; }).ToList(),
                post, result.PiezoNoiseRms, data.PiezoRate, 5.0);
            result.PiezoDurationMs = duration.DurationMs;
            result.PiezoDurationTruncated = duration.Truncated;
        }

        private static void AnalyzeMpu(EventData data, AnalysisResult result)
        {
            List<SampleData> valid = data.Samples.Where(delegate(SampleData s) { return s.MpuValid; }).ToList();
            List<SampleData> pre = valid.Where(delegate(SampleData s) { return s.TimeUs < 0; }).ToList();
            if (pre.Count == 0 || data.AccelLsbPerG <= 0.0)
            {
                result.DominantAxis = "N/A";
                result.MpuSnrDb = Double.NegativeInfinity;
                return;
            }

            result.MpuMeanAxRaw = pre.Average(delegate(SampleData s) { return (double)s.Ax; });
            result.MpuMeanAyRaw = pre.Average(delegate(SampleData s) { return (double)s.Ay; });
            result.MpuMeanAzRaw = pre.Average(delegate(SampleData s) { return (double)s.Az; });
            result.MpuMeanGxRaw = pre.Average(delegate(SampleData s) { return (double)s.Gx; });
            result.MpuMeanGyRaw = pre.Average(delegate(SampleData s) { return (double)s.Gy; });
            result.MpuMeanGzRaw = pre.Average(delegate(SampleData s) { return (double)s.Gz; });

            List<double> preX = pre.Select(delegate(SampleData s) { return (s.Ax - result.MpuMeanAxRaw) / data.AccelLsbPerG; }).ToList();
            List<double> preY = pre.Select(delegate(SampleData s) { return (s.Ay - result.MpuMeanAyRaw) / data.AccelLsbPerG; }).ToList();
            List<double> preZ = pre.Select(delegate(SampleData s) { return (s.Az - result.MpuMeanAzRaw) / data.AccelLsbPerG; }).ToList();

            List<SampleData> postRows = valid.Where(delegate(SampleData s) { return s.TimeUs >= 0; }).ToList();
            List<double> postX = postRows.Select(delegate(SampleData s) { return (s.Ax - result.MpuMeanAxRaw) / data.AccelLsbPerG; }).ToList();
            List<double> postY = postRows.Select(delegate(SampleData s) { return (s.Ay - result.MpuMeanAyRaw) / data.AccelLsbPerG; }).ToList();
            List<double> postZ = postRows.Select(delegate(SampleData s) { return (s.Az - result.MpuMeanAzRaw) / data.AccelLsbPerG; }).ToList();

            double eventRmsX = Rms(FirstMilliseconds(postRows, postX, 300000));
            double eventRmsY = Rms(FirstMilliseconds(postRows, postY, 300000));
            double eventRmsZ = Rms(FirstMilliseconds(postRows, postZ, 300000));

            List<double> selectedPre;
            List<double> selectedPost;
            if (eventRmsX >= eventRmsY && eventRmsX >= eventRmsZ)
            {
                result.DominantAxis = "X";
                selectedPre = preX;
                selectedPost = postX;
            }
            else if (eventRmsY >= eventRmsX && eventRmsY >= eventRmsZ)
            {
                result.DominantAxis = "Y";
                selectedPre = preY;
                selectedPost = postY;
            }
            else
            {
                result.DominantAxis = "Z";
                selectedPre = preZ;
                selectedPost = postZ;
            }

            result.MpuNoiseRmsG = Math.Max(0.000001, Rms(selectedPre));
            if (selectedPost.Count == 0) selectedPost.Add(0.0);
            result.MpuPositivePeakG = selectedPost.Max();
            result.MpuNegativePeakG = selectedPost.Min();
            result.MpuAbsolutePeakG = selectedPost.Max(delegate(double v) { return Math.Abs(v); });
            result.MpuPeakToPeakG = result.MpuPositivePeakG - result.MpuNegativePeakG;
            result.MpuVectorPeakG = postX.Select(delegate(double x, int i)
            {
                return Math.Sqrt(x * x + postY[i] * postY[i] + postZ[i] * postZ[i]);
            }).DefaultIfEmpty(0.0).Max();
            result.MpuSaturated = valid.Any(delegate(SampleData s)
            {
                return Math.Abs((int)s.Ax) >= 32760 || Math.Abs((int)s.Ay) >= 32760 || Math.Abs((int)s.Az) >= 32760 ||
                       Math.Abs((int)s.Gx) >= 32760 || Math.Abs((int)s.Gy) >= 32760 || Math.Abs((int)s.Gz) >= 32760;
            });

            double mpuEventPower = MeanSquare(FirstMilliseconds(postRows, selectedPost, 300000));
            result.MpuSnrDb = CalculateSnr(mpuEventPower, result.MpuNoiseRmsG * result.MpuNoiseRmsG);

            DurationResult duration = FindDuration(
                postRows.Select(delegate(SampleData s) { return s.TimeUs / 1000.0; }).ToList(),
                selectedPost, result.MpuNoiseRmsG, data.MpuRate, 10.0);
            result.MpuDurationMs = duration.DurationMs;
            result.MpuDurationTruncated = duration.Truncated;

            result.DominantFrequencyHz = FindDominantFrequency(selectedPost, data.MpuRate, 5.0, 250.0);
            result.PeriodMs = result.DominantFrequencyHz > 0.0 ? 1000.0 / result.DominantFrequencyHz : 0.0;
        }

        private static List<double> FirstMilliseconds(List<SampleData> rows, List<double> values, int maximumUs)
        {
            List<double> output = new List<double>();
            int count = Math.Min(rows.Count, values.Count);
            for (int i = 0; i < count; ++i)
            {
                if (rows[i].TimeUs <= maximumUs) output.Add(values[i]);
            }
            return output;
        }

        private static double CalculateDropRate(EventData data)
        {
            int inferredMissingPiezo = 0;
            for (int i = 1; i < data.Samples.Count; ++i)
            {
                int difference = data.Samples[i].TimeUs - data.Samples[i - 1].TimeUs;
                int expected = data.PiezoRate > 0 ? 1000000 / data.PiezoRate : 500;
                if (difference > expected) inferredMissingPiezo += Math.Max(0, difference / expected - 1);
            }

            int missingRows = Math.Max(0, data.ExpectedSamples - data.Samples.Count);
            int missingPiezo = Math.Max(data.FirmwareMissedTicks, inferredMissingPiezo) + missingRows;
            double piezoRate = missingPiezo / (double)Math.Max(1, data.ExpectedSamples + missingPiezo);

            int expectedMpu = (data.Samples.Count + 1) / 2;
            int actualMpu = data.Samples.Count(delegate(SampleData s) { return s.MpuValid; });
            int missingMpu = Math.Max(data.FirmwareMpuFailures, Math.Max(0, expectedMpu - actualMpu));
            double mpuRate = data.MpuAvailable ? missingMpu / (double)Math.Max(1, expectedMpu) : 1.0;
            return Math.Max(piezoRate, mpuRate);
        }

        private static double FindDominantFrequency(List<double> values, int sampleRate, double minimumHz, double maximumHz)
        {
            int n = values.Count;
            if (n < 32 || sampleRate <= 0) return 0.0;
            double mean = Mean(values);
            int firstBin = Math.Max(1, (int)Math.Ceiling(minimumHz * n / sampleRate));
            int lastBin = Math.Min(n / 2, (int)Math.Floor(maximumHz * n / sampleRate));
            double bestPower = -1.0;
            int bestBin = 0;

            for (int k = firstBin; k <= lastBin; ++k)
            {
                double real = 0.0;
                double imaginary = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    double window = n > 1 ? 0.5 - 0.5 * Math.Cos(2.0 * Math.PI * i / (n - 1)) : 1.0;
                    double value = (values[i] - mean) * window;
                    double angle = 2.0 * Math.PI * k * i / n;
                    real += value * Math.Cos(angle);
                    imaginary -= value * Math.Sin(angle);
                }
                double power = real * real + imaginary * imaginary;
                if (power > bestPower)
                {
                    bestPower = power;
                    bestBin = k;
                }
            }

            return bestBin > 0 ? bestBin * (double)sampleRate / n : 0.0;
        }

        private sealed class DurationResult
        {
            public double DurationMs;
            public bool Truncated;
        }

        private static DurationResult FindDuration(List<double> timesMs, List<double> values, double noiseRms,
                                                   int sampleRate, double envelopeWindowMs)
        {
            DurationResult result = new DurationResult();
            int count = Math.Min(timesMs.Count, values.Count);
            if (count == 0 || sampleRate <= 0)
            {
                result.Truncated = true;
                return result;
            }

            int window = Math.Max(1, (int)Math.Round(sampleRate * envelopeWindowMs / 1000.0));
            double[] envelope = new double[count];
            double running = 0.0;
            int maximumIndex = 0;
            for (int i = 0; i < count; ++i)
            {
                running += values[i] * values[i];
                if (i >= window) running -= values[i - window] * values[i - window];
                int divisor = Math.Min(i + 1, window);
                envelope[i] = Math.Sqrt(Math.Max(0.0, running / divisor));
                if (envelope[i] > envelope[maximumIndex]) maximumIndex = i;
            }

            double threshold = Math.Max(noiseRms * 3.0, 0.000001);
            int quietSamples = Math.Max(1, (int)Math.Round(sampleRate * 0.030));
            int quietRun = 0;
            double earliestMs = 20.0;
            for (int i = maximumIndex; i < count; ++i)
            {
                if (timesMs[i] < earliestMs) continue;
                if (envelope[i] < threshold) ++quietRun; else quietRun = 0;
                if (quietRun >= quietSamples)
                {
                    int endIndex = i - quietSamples + 1;
                    result.DurationMs = Math.Max(0.0, timesMs[endIndex]);
                    result.Truncated = false;
                    return result;
                }
            }

            result.DurationMs = Math.Max(0.0, timesMs[count - 1]);
            result.Truncated = true;
            return result;
        }

        private static string BuildFailureReason(AnalysisResult result, double piezoLimit, double mpuLimit)
        {
            List<string> reasons = new List<string>();
            if (result.PiezoSaturated) reasons.Add("压电饱和");
            if (!IsFinite(result.PiezoSnrDb) || result.PiezoSnrDb < piezoLimit) reasons.Add("压电SNR不足");
            if (result.MpuSaturated) reasons.Add("MPU饱和");
            if (!IsFinite(result.MpuSnrDb) || result.MpuSnrDb < mpuLimit) reasons.Add("MPU SNR不足");
            if (result.DropRatePercent >= 1.0) reasons.Add("丢点率过高");
            return reasons.Count > 0 ? String.Join("、", reasons.ToArray()) : "未知原因";
        }

        private static double CalculateSnr(double eventPower, double noisePower)
        {
            noisePower = Math.Max(noisePower, 1e-18);
            double signalPower = eventPower - noisePower;
            if (signalPower <= 0.0) return Double.NegativeInfinity;
            return 10.0 * Math.Log10(signalPower / noisePower);
        }

        private static double Mean(IEnumerable<double> values)
        {
            double sum = 0.0;
            long count = 0;
            foreach (double value in values) { sum += value; ++count; }
            return count > 0 ? sum / count : 0.0;
        }

        private static double MeanSquare(IEnumerable<double> values)
        {
            double sum = 0.0;
            long count = 0;
            foreach (double value in values) { sum += value * value; ++count; }
            return count > 0 ? sum / count : 0.0;
        }

        private static double Rms(IEnumerable<double> values)
        {
            return Math.Sqrt(Math.Max(0.0, MeanSquare(values)));
        }

        private static bool IsFinite(double value)
        {
            return !Double.IsNaN(value) && !Double.IsInfinity(value);
        }
    }
}
