using System;

namespace TingguSignalWorkbench
{
    internal static class SelfTest
    {
        public static void Run()
        {
            EventData data = BuildSyntheticEvent();
            data.CalculatedChecksum = SerialProtocol.CalculateChecksum(data);
            data.DeclaredChecksum = data.CalculatedChecksum;
            data.IntegrityValid = true;
            data.IntegrityMessage = "完整";

            AnalysisResult result = SignalAnalyzer.Analyze(data, 10.0, 6.0);
            Assert(result.DominantAxis == "Z", "主振动轴应为Z，实际为" + result.DominantAxis);
            Assert(Math.Abs(result.DominantFrequencyHz - 50.0) < 3.0,
                   "主频应接近50 Hz，实际为" + result.DominantFrequencyHz.ToString("F3"));
            Assert(result.PiezoSnrDb > 10.0, "压电SNR应超过10 dB");
            Assert(result.MpuSnrDb > 6.0, "MPU SNR应超过6 dB");
            Assert(result.PiezoDurationMs > 20.0, "压电持续时间应大于20 ms");
            Assert(result.MpuDurationMs > 20.0, "MPU持续时间应大于20 ms");
            Assert(result.DropRatePercent < 0.001, "合成数据不应丢点");
            Assert(result.DualSensorValid, "合成事件应通过双传感器质量判定：" + result.QualityText);

            // Constructing the whole form catches missing WinForms/Charting assemblies
            // and layout-time exceptions without opening a window.
            Environment.SetEnvironmentVariable("TINGGU_OUTPUT_DIR",
                System.IO.Path.Combine(System.IO.Path.GetTempPath(), "TingguSignalWorkbenchSelfTest"));
            using (MainForm form = new MainForm())
            {
                Assert(form.Text.Contains("双传感器"), "主界面未能正确构造");
            }
        }

        private static EventData BuildSyntheticEvent()
        {
            EventData data = new EventData();
            data.EventId = 1;
            data.ExpectedSamples = 2000;
            data.PreSamples = 200;
            data.PostSamples = 1800;
            data.PiezoRate = 2000;
            data.MpuRate = 1000;
            data.FirmwarePiezoBaseline = 1870.0;
            data.TriggerThreshold = 80;
            data.MpuAvailable = true;
            data.AccelLsbPerG = 4096.0;
            data.GyroLsbPerDps = 65.5;

            for (int i = 0; i < 2000; ++i)
            {
                int timeUs = (i - 200) * 500;
                double timeSeconds = timeUs / 1000000.0;
                int piezoNoise = ((i * 17) % 9) - 4;
                double piezoSignal = 0.0;
                if (timeUs >= 0)
                    piezoSignal = 650.0 * Math.Exp(-timeSeconds / 0.045) * Math.Cos(2.0 * Math.PI * 115.0 * timeSeconds);

                SampleData sample = new SampleData();
                sample.Index = i;
                sample.TimeUs = timeUs;
                sample.PiezoRaw = (ushort)Math.Max(0, Math.Min(4095, (int)Math.Round(1870.0 + piezoNoise + piezoSignal)));
                sample.MpuValid = (i % 2) == 0;
                if (sample.MpuValid)
                {
                    int noiseX = ((i * 5) % 7) - 3;
                    int noiseY = ((i * 7) % 9) - 4;
                    int noiseZ = ((i * 11) % 11) - 5;
                    double zSignalG = 0.0;
                    double xSignalG = 0.0;
                    if (timeUs >= 0)
                    {
                        zSignalG = 0.22 * Math.Exp(-timeSeconds / 0.120) * Math.Sin(2.0 * Math.PI * 50.0 * timeSeconds);
                        xSignalG = 0.015 * Math.Exp(-timeSeconds / 0.080) * Math.Sin(2.0 * Math.PI * 80.0 * timeSeconds);
                    }
                    sample.Ax = (short)Math.Round(noiseX + xSignalG * 4096.0);
                    sample.Ay = (short)noiseY;
                    sample.Az = (short)Math.Round(4096.0 + noiseZ + zSignalG * 4096.0);
                    sample.Gx = (short)noiseX;
                    sample.Gy = (short)noiseY;
                    sample.Gz = (short)noiseZ;
                }
                data.Samples.Add(sample);
            }
            return data;
        }

        private static void Assert(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
        }
    }
}
