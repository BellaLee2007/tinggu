using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace TingguSignalWorkbench
{
    internal sealed class CsvExporter
    {
        private readonly string outputDirectory;
        private readonly Encoding encoding = new UTF8Encoding(true);

        public CsvExporter(string directory)
        {
            outputDirectory = directory;
            Directory.CreateDirectory(outputDirectory);
        }

        public string OutputDirectory { get { return outputDirectory; } }

        public string Export(EventData data, AnalysisResult analysis, ExperimentMetadata metadata)
        {
            string timestamp = DateTime.Now.ToString("yyyyMMdd_HHmmss_fff", CultureInfo.InvariantCulture);
            string fileName = String.Format(CultureInfo.InvariantCulture, "event_{0:D4}_{1}_{2}.csv",
                                            data.EventId, SafeFilePart(metadata.DisplayLabel), timestamp);
            string path = Path.Combine(outputDirectory, fileName);

            using (StreamWriter writer = new StreamWriter(path, false, encoding))
            {
                writer.WriteLine("event_id,label,time_us,piezo_raw,piezo_delta,mpu_valid,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps");
                foreach (SampleData sample in data.Samples)
                {
                    writer.Write(data.EventId.ToString(CultureInfo.InvariantCulture));
                    writer.Write(','); writer.Write(Quote(metadata.DisplayLabel));
                    writer.Write(','); writer.Write(sample.TimeUs.ToString(CultureInfo.InvariantCulture));
                    writer.Write(','); writer.Write(sample.PiezoRaw.ToString(CultureInfo.InvariantCulture));
                    writer.Write(','); writer.Write((sample.PiezoRaw - analysis.PiezoBaseline).ToString("F4", CultureInfo.InvariantCulture));
                    writer.Write(','); writer.Write(sample.MpuValid ? "1" : "0");

                    if (sample.MpuValid)
                    {
                        writer.Write(','); writer.Write((sample.Ax / data.AccelLsbPerG).ToString("F7", CultureInfo.InvariantCulture));
                        writer.Write(','); writer.Write((sample.Ay / data.AccelLsbPerG).ToString("F7", CultureInfo.InvariantCulture));
                        writer.Write(','); writer.Write((sample.Az / data.AccelLsbPerG).ToString("F7", CultureInfo.InvariantCulture));
                        writer.Write(','); writer.Write((sample.Gx / data.GyroLsbPerDps).ToString("F6", CultureInfo.InvariantCulture));
                        writer.Write(','); writer.Write((sample.Gy / data.GyroLsbPerDps).ToString("F6", CultureInfo.InvariantCulture));
                        writer.Write(','); writer.Write((sample.Gz / data.GyroLsbPerDps).ToString("F6", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        writer.Write(",,,,,,");
                    }
                    writer.WriteLine();
                }
            }

            AppendSummary(data, analysis, metadata, fileName);
            return path;
        }

        private void AppendSummary(EventData data, AnalysisResult result, ExperimentMetadata metadata, string sampleFileName)
        {
            string path = Path.Combine(outputDirectory, "summary.csv");
            bool writeHeader = !File.Exists(path) || new FileInfo(path).Length == 0;
            using (StreamWriter writer = new StreamWriter(path, true, encoding))
            {
                if (writeHeader)
                {
                    writer.WriteLine("event_id,captured_at,specimen_id,tightness_label,torque,strike_method,notes," +
                        "piezo_peak,piezo_snr_db,piezo_duration_ms,mpu_peak_g,mpu_vector_peak_g,mpu_snr_db,mpu_duration_ms," +
                        "dominant_axis,dominant_frequency_hz,period_ms,piezo_saturated,mpu_saturated,drop_rate_percent," +
                        "quality,integrity,sample_file,piezo_rate_hz,mpu_rate_hz,trigger_threshold,accel_lsb_per_g,gyro_lsb_per_dps");
                }

                string[] fields = new string[]
                {
                    data.EventId.ToString(CultureInfo.InvariantCulture),
                    DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture),
                    metadata.SpecimenId, metadata.TightnessLabel, metadata.Torque, metadata.StrikeMethod, metadata.Notes,
                    result.PiezoAbsolutePeak.ToString("F4", CultureInfo.InvariantCulture),
                    FormatNumber(result.PiezoSnrDb, "F3"),
                    result.PiezoDurationMs.ToString("F3", CultureInfo.InvariantCulture),
                    result.MpuAbsolutePeakG.ToString("F7", CultureInfo.InvariantCulture),
                    result.MpuVectorPeakG.ToString("F7", CultureInfo.InvariantCulture),
                    FormatNumber(result.MpuSnrDb, "F3"),
                    result.MpuDurationMs.ToString("F3", CultureInfo.InvariantCulture),
                    result.DominantAxis,
                    result.DominantFrequencyHz.ToString("F4", CultureInfo.InvariantCulture),
                    result.PeriodMs.ToString("F4", CultureInfo.InvariantCulture),
                    result.PiezoSaturated ? "1" : "0", result.MpuSaturated ? "1" : "0",
                    result.DropRatePercent.ToString("F4", CultureInfo.InvariantCulture),
                    result.QualityText, data.IntegrityMessage, sampleFileName,
                    data.PiezoRate.ToString(CultureInfo.InvariantCulture), data.MpuRate.ToString(CultureInfo.InvariantCulture),
                    data.TriggerThreshold.ToString(CultureInfo.InvariantCulture),
                    data.AccelLsbPerG.ToString("F3", CultureInfo.InvariantCulture),
                    data.GyroLsbPerDps.ToString("F3", CultureInfo.InvariantCulture)
                };

                for (int i = 0; i < fields.Length; ++i)
                {
                    if (i > 0) writer.Write(',');
                    writer.Write(Quote(fields[i]));
                }
                writer.WriteLine();
            }
        }

        private static string FormatNumber(double value, string format)
        {
            if (Double.IsNaN(value)) return "NaN";
            if (Double.IsPositiveInfinity(value)) return "Infinity";
            if (Double.IsNegativeInfinity(value)) return "-Infinity";
            return value.ToString(format, CultureInfo.InvariantCulture);
        }

        private static string Quote(string value)
        {
            string safe = value ?? "";
            return "\"" + safe.Replace("\"", "\"\"") + "\"";
        }

        private static string SafeFilePart(string value)
        {
            string result = value ?? "unlabeled";
            foreach (char invalid in Path.GetInvalidFileNameChars()) result = result.Replace(invalid, '_');
            result = result.Trim();
            if (result.Length == 0) result = "unlabeled";
            if (result.Length > 40) result = result.Substring(0, 40);
            return result;
        }
    }
}
