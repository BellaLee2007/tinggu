using System;
using System.Globalization;

namespace TingguSignalWorkbench
{
    internal sealed class SerialProtocol
    {
        private EventData current;
        private int nextIndex;
        private bool rowError;
        private string rowErrorMessage;

        public event Action<EventData> EventCompleted;
        public event Action<string> StatusLine;
        public event Action<string> ProtocolError;

        public void Reset()
        {
            current = null;
            nextIndex = 0;
            rowError = false;
            rowErrorMessage = "";
        }

        public void AcceptLine(string input)
        {
            string line = input == null ? "" : input.Trim();
            if (line.Length == 0) return;

            try
            {
                if (line.StartsWith("#BEGIN,"))
                    BeginEvent(line);
                else if (line.StartsWith("D,"))
                    AddDataRow(line);
                else if (line.StartsWith("#END,"))
                    EndEvent(line);
                else if (StatusLine != null)
                    StatusLine(line);
            }
            catch (Exception ex)
            {
                if (ProtocolError != null) ProtocolError("协议解析失败：" + ex.Message + "；原文：" + line);
                Reset();
            }
        }

        private void BeginEvent(string line)
        {
            string[] fields = line.Split(',');
            if (fields.Length != 14) throw new FormatException("#BEGIN字段数应为14，实际为" + fields.Length);

            current = new EventData();
            current.EventId = ParseInt(fields[1]);
            current.ExpectedSamples = ParseInt(fields[2]);
            current.PreSamples = ParseInt(fields[3]);
            current.PostSamples = ParseInt(fields[4]);
            current.PiezoRate = ParseInt(fields[5]);
            current.MpuRate = ParseInt(fields[6]);
            current.FirmwarePiezoBaseline = ParseDouble(fields[7]);
            current.TriggerThreshold = ParseInt(fields[8]);
            current.MpuAvailable = ParseInt(fields[9]) != 0;
            current.AccelLsbPerG = ParseDouble(fields[10]);
            current.GyroLsbPerDps = ParseDouble(fields[11]);
            current.FirmwareMissedTicks = ParseInt(fields[12]);
            current.FirmwareMpuFailures = ParseInt(fields[13]);
            nextIndex = 0;
            rowError = false;
            rowErrorMessage = "";
            if (StatusLine != null) StatusLine("正在接收事件 " + current.EventId + "（预计 " + current.ExpectedSamples + " 行）");
        }

        private void AddDataRow(string line)
        {
            if (current == null) throw new FormatException("在#BEGIN之前收到数据行");
            string[] fields = line.Split(',');
            if (fields.Length != 11) throw new FormatException("D行字段数应为11，实际为" + fields.Length);

            SampleData sample = new SampleData();
            sample.Index = ParseInt(fields[1]);
            sample.TimeUs = ParseInt(fields[2]);
            sample.PiezoRaw = checked((ushort)ParseInt(fields[3]));
            sample.MpuValid = ParseInt(fields[4]) != 0;
            sample.Ax = checked((short)ParseInt(fields[5]));
            sample.Ay = checked((short)ParseInt(fields[6]));
            sample.Az = checked((short)ParseInt(fields[7]));
            sample.Gx = checked((short)ParseInt(fields[8]));
            sample.Gy = checked((short)ParseInt(fields[9]));
            sample.Gz = checked((short)ParseInt(fields[10]));

            if (sample.Index != nextIndex)
            {
                rowError = true;
                rowErrorMessage = "行号不连续：期望" + nextIndex + "，收到" + sample.Index;
            }
            ++nextIndex;
            current.Samples.Add(sample);
        }

        private void EndEvent(string line)
        {
            if (current == null) throw new FormatException("在#BEGIN之前收到#END");
            string[] fields = line.Split(',');
            if (fields.Length != 4) throw new FormatException("#END字段数应为4");

            int endEventId = ParseInt(fields[1]);
            int declaredCount = ParseInt(fields[2]);
            uint declaredChecksum = UInt32.Parse(fields[3], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            current.DeclaredChecksum = declaredChecksum;
            current.CalculatedChecksum = CalculateChecksum(current);

            bool eventIdOk = endEventId == current.EventId;
            bool countOk = declaredCount == current.Samples.Count && current.ExpectedSamples == current.Samples.Count;
            bool checksumOk = current.DeclaredChecksum == current.CalculatedChecksum;
            current.IntegrityValid = eventIdOk && countOk && checksumOk && !rowError;

            if (current.IntegrityValid)
                current.IntegrityMessage = "完整";
            else if (!eventIdOk)
                current.IntegrityMessage = "事件编号不一致";
            else if (!countOk)
                current.IntegrityMessage = "样本数不一致";
            else if (rowError)
                current.IntegrityMessage = rowErrorMessage;
            else
                current.IntegrityMessage = "校验值不一致";

            EventData completed = current;
            Reset();
            if (EventCompleted != null) EventCompleted(completed);
        }

        public static uint CalculateChecksum(EventData data)
        {
            uint checksum = 2166136261U;
            foreach (SampleData sample in data.Samples)
            {
                checksum = AddUInt32(checksum, unchecked((uint)sample.TimeUs));
                checksum = AddUInt16(checksum, sample.PiezoRaw);
                checksum = AddByte(checksum, sample.MpuValid ? (byte)1 : (byte)0);
                checksum = AddUInt16(checksum, unchecked((ushort)sample.Ax));
                checksum = AddUInt16(checksum, unchecked((ushort)sample.Ay));
                checksum = AddUInt16(checksum, unchecked((ushort)sample.Az));
                checksum = AddUInt16(checksum, unchecked((ushort)sample.Gx));
                checksum = AddUInt16(checksum, unchecked((ushort)sample.Gy));
                checksum = AddUInt16(checksum, unchecked((ushort)sample.Gz));
            }
            return checksum;
        }

        private static uint AddByte(uint checksum, byte value)
        {
            return unchecked((checksum ^ value) * 16777619U);
        }

        private static uint AddUInt16(uint checksum, ushort value)
        {
            checksum = AddByte(checksum, (byte)(value & 0xFF));
            checksum = AddByte(checksum, (byte)((value >> 8) & 0xFF));
            return checksum;
        }

        private static uint AddUInt32(uint checksum, uint value)
        {
            checksum = AddByte(checksum, (byte)(value & 0xFF));
            checksum = AddByte(checksum, (byte)((value >> 8) & 0xFF));
            checksum = AddByte(checksum, (byte)((value >> 16) & 0xFF));
            checksum = AddByte(checksum, (byte)((value >> 24) & 0xFF));
            return checksum;
        }

        private static int ParseInt(string text)
        {
            return Int32.Parse(text, NumberStyles.Integer, CultureInfo.InvariantCulture);
        }

        private static double ParseDouble(string text)
        {
            return Double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture);
        }
    }
}
