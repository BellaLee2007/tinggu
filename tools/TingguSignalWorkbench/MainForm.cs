using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO.Ports;
using System.Linq;
using System.Threading;
using System.Windows.Forms;
using System.Windows.Forms.DataVisualization.Charting;

namespace TingguSignalWorkbench
{
    internal sealed class MainForm : Form
    {
        private readonly ComboBox portBox = new ComboBox();
        private readonly Button refreshButton = new Button();
        private readonly Button connectButton = new Button();
        private readonly Button recalibrateButton = new Button();
        private readonly Button armButton = new Button();
        private readonly Button captureButton = new Button();
        private readonly Button openFolderButton = new Button();
        private readonly Label stateLabel = new Label();
        private readonly Label mpuLabel = new Label();
        private readonly Label samplingLabel = new Label();

        private readonly TextBox specimenBox = new TextBox();
        private readonly TextBox tightnessBox = new TextBox();
        private readonly TextBox torqueBox = new TextBox();
        private readonly ComboBox strikeBox = new ComboBox();
        private readonly TextBox notesBox = new TextBox();
        private readonly NumericUpDown piezoSnrBox = new NumericUpDown();
        private readonly NumericUpDown mpuSnrBox = new NumericUpDown();
        private readonly ComboBox mpuAxisBox = new ComboBox();
        private readonly CheckBox overlayBox = new CheckBox();

        private readonly Chart piezoChart;
        private readonly Chart mpuChart;
        private readonly DataGridView metricsGrid = new DataGridView();
        private readonly TextBox logBox = new TextBox();
        private readonly Label verdictLabel = new Label();

        private readonly SerialProtocol protocol = new SerialProtocol();
        private readonly CsvExporter exporter;
        private readonly List<CompletedEvent> history = new List<CompletedEvent>();
        private readonly object serialLock = new object();
        private SerialPort serialPort;
        private Thread readerThread;
        private volatile bool readerRunning;

        public MainForm()
        {
            Text = "听固：双传感器敲击验证工作台（本工具不直接判断螺丝松紧）";
            MinimumSize = new Size(1100, 720);
            Size = new Size(1450, 900);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Microsoft YaHei UI", 9F);

            string configuredOutput = Environment.GetEnvironmentVariable("TINGGU_OUTPUT_DIR");
            string outputDirectory = String.IsNullOrWhiteSpace(configuredOutput)
                ? System.IO.Path.GetFullPath(System.IO.Path.Combine(
                    AppDomain.CurrentDomain.BaseDirectory, "..", "..", "data", "signal_validator"))
                : System.IO.Path.GetFullPath(configuredOutput);
            exporter = new CsvExporter(outputDirectory);

            piezoChart = CreateChart("压电片：基准校正后的 ADC 计数", "ADC计数");
            mpuChart = CreateChart("MPU-6050：动态加速度（已减去静止均值）", "加速度 (g)");
            BuildLayout();
            WireEvents();
            RefreshPorts();
            SetConnectedState(false);
            AppendLog("工具已启动。先填写固件顶部的SDA/SCL并上传，再选择COM口连接。", Color.DimGray);
            AppendLog("本阶段只验证信号质量和重复性，不会根据一张波形判断螺丝松紧。", Color.DarkOrange);
        }

        private void BuildLayout()
        {
            TableLayoutPanel root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.RowCount = 4;
            root.ColumnCount = 1;
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 78));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 190));
            Controls.Add(root);

            FlowLayoutPanel commands = new FlowLayoutPanel();
            commands.Dock = DockStyle.Fill;
            commands.Padding = new Padding(7, 7, 4, 4);
            commands.WrapContents = false;
            commands.AutoScroll = true;
            commands.Controls.Add(MakeLabel("串口", 35));
            portBox.Width = 100;
            portBox.DropDownStyle = ComboBoxStyle.DropDownList;
            commands.Controls.Add(portBox);
            SetButton(refreshButton, "刷新", 58);
            SetButton(connectButton, "连接", 70);
            SetButton(recalibrateButton, "重新标定", 82);
            SetButton(armButton, "下一次采集 / ARM", 132);
            SetButton(captureButton, "手动采集", 82);
            SetButton(openFolderButton, "打开CSV目录", 100);
            commands.Controls.Add(refreshButton);
            commands.Controls.Add(connectButton);
            commands.Controls.Add(recalibrateButton);
            commands.Controls.Add(armButton);
            commands.Controls.Add(captureButton);
            commands.Controls.Add(openFolderButton);
            stateLabel.AutoSize = true;
            stateLabel.Margin = new Padding(15, 7, 3, 3);
            stateLabel.Text = "状态：未连接";
            stateLabel.Font = new Font(Font, FontStyle.Bold);
            commands.Controls.Add(stateLabel);
            mpuLabel.AutoSize = true;
            mpuLabel.Margin = new Padding(12, 7, 3, 3);
            mpuLabel.Text = "MPU：未知";
            commands.Controls.Add(mpuLabel);
            samplingLabel.AutoSize = true;
            samplingLabel.Margin = new Padding(12, 7, 3, 3);
            samplingLabel.Text = "采样：压电2000 / MPU1000 Hz";
            commands.Controls.Add(samplingLabel);
            root.Controls.Add(commands, 0, 0);

            FlowLayoutPanel metadata = new FlowLayoutPanel();
            metadata.Dock = DockStyle.Fill;
            metadata.Padding = new Padding(8, 3, 4, 3);
            metadata.AutoScroll = true;
            metadata.WrapContents = true;
            AddLabeledControl(metadata, "试件编号", specimenBox, 95);
            AddLabeledControl(metadata, "松紧描述", tightnessBox, 110);
            AddLabeledControl(metadata, "扭矩(N·m)", torqueBox, 70);
            strikeBox.DropDownStyle = ComboBoxStyle.DropDown;
            strikeBox.Items.AddRange(new object[] { "手敲", "标准机构", "轻敲", "中敲", "重敲" });
            strikeBox.Text = "手敲";
            AddLabeledControl(metadata, "敲击方式", strikeBox, 90);
            AddLabeledControl(metadata, "备注", notesBox, 160);
            ConfigureSnrBox(piezoSnrBox, 10);
            ConfigureSnrBox(mpuSnrBox, 6);
            AddLabeledControl(metadata, "压电SNR≥", piezoSnrBox, 55);
            AddLabeledControl(metadata, "MPU SNR≥", mpuSnrBox, 55);
            mpuAxisBox.DropDownStyle = ComboBoxStyle.DropDownList;
            mpuAxisBox.Items.AddRange(new object[] { "主振动轴", "X", "Y", "Z", "动态合量" });
            mpuAxisBox.SelectedIndex = 0;
            AddLabeledControl(metadata, "MPU显示", mpuAxisBox, 100);
            overlayBox.Text = "叠加最近5次";
            overlayBox.Checked = true;
            overlayBox.AutoSize = true;
            overlayBox.Margin = new Padding(10, 7, 3, 3);
            metadata.Controls.Add(overlayBox);
            root.Controls.Add(metadata, 0, 1);

            SplitContainer charts = new SplitContainer();
            charts.Dock = DockStyle.Fill;
            charts.Orientation = Orientation.Horizontal;
            charts.SplitterDistance = 280;
            charts.Panel1.Controls.Add(piezoChart);
            charts.Panel2.Controls.Add(mpuChart);
            root.Controls.Add(charts, 0, 2);

            SplitContainer bottom = new SplitContainer();
            bottom.Dock = DockStyle.Fill;
            bottom.Orientation = Orientation.Vertical;
            bottom.SplitterDistance = 720;
            root.Controls.Add(bottom, 0, 3);

            TableLayoutPanel metricPanel = new TableLayoutPanel();
            metricPanel.Dock = DockStyle.Fill;
            metricPanel.RowCount = 2;
            metricPanel.ColumnCount = 1;
            metricPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
            metricPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            verdictLabel.Dock = DockStyle.Fill;
            verdictLabel.TextAlign = ContentAlignment.MiddleLeft;
            verdictLabel.Padding = new Padding(8, 0, 0, 0);
            verdictLabel.Font = new Font(Font.FontFamily, 11F, FontStyle.Bold);
            verdictLabel.Text = "等待第一次敲击";
            metricPanel.Controls.Add(verdictLabel, 0, 0);
            ConfigureMetricsGrid();
            metricPanel.Controls.Add(metricsGrid, 0, 1);
            bottom.Panel1.Controls.Add(metricPanel);

            logBox.Dock = DockStyle.Fill;
            logBox.Multiline = true;
            logBox.ReadOnly = true;
            logBox.ScrollBars = ScrollBars.Vertical;
            logBox.BackColor = Color.White;
            logBox.Font = new Font("Consolas", 9F);
            bottom.Panel2.Controls.Add(logBox);
        }

        private void WireEvents()
        {
            refreshButton.Click += delegate { RefreshPorts(); };
            connectButton.Click += delegate { if (IsConnected) Disconnect(); else Connect(); };
            recalibrateButton.Click += delegate { SendCommand("RECALIBRATE"); };
            armButton.Click += delegate { SendCommand("ARM"); };
            captureButton.Click += delegate { SendCommand("CAPTURE"); };
            openFolderButton.Click += delegate
            {
                try { Process.Start("explorer.exe", "\"" + exporter.OutputDirectory + "\""); }
                catch (Exception ex) { AppendLog("无法打开目录：" + ex.Message, Color.Firebrick); }
            };
            mpuAxisBox.SelectedIndexChanged += delegate { RenderCharts(); };
            overlayBox.CheckedChanged += delegate { RenderCharts(); };
            piezoChart.DoubleClick += delegate { ResetZoom(piezoChart); };
            mpuChart.DoubleClick += delegate { ResetZoom(mpuChart); };
            FormClosing += delegate { Disconnect(); };

            protocol.StatusLine += delegate(string line) { Ui(delegate { HandleStatusLine(line); }); };
            protocol.ProtocolError += delegate(string error) { Ui(delegate { AppendLog(error, Color.Firebrick); }); };
            protocol.EventCompleted += delegate(EventData data) { Ui(delegate { HandleCompletedEvent(data); }); };
        }

        private bool IsConnected
        {
            get { return serialPort != null && serialPort.IsOpen; }
        }

        private void RefreshPorts()
        {
            string previous = portBox.SelectedItem as string;
            string[] ports = SerialPort.GetPortNames().OrderBy(delegate(string value) { return value; }).ToArray();
            portBox.Items.Clear();
            portBox.Items.AddRange(ports);
            if (previous != null && ports.Contains(previous)) portBox.SelectedItem = previous;
            else if (ports.Length > 0) portBox.SelectedIndex = 0;
            AppendLog("检测到串口：" + (ports.Length == 0 ? "无" : String.Join("、", ports)), Color.DimGray);
        }

        private void Connect()
        {
            if (portBox.SelectedItem == null)
            {
                MessageBox.Show(this, "没有可选串口。请先确认USB线和驱动，然后点“刷新”。", "无法连接",
                                MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try
            {
                serialPort = new SerialPort(portBox.SelectedItem.ToString(), 460800, Parity.None, 8, StopBits.One);
                serialPort.NewLine = "\n";
                serialPort.ReadTimeout = 500;
                serialPort.WriteTimeout = 1000;
                serialPort.DtrEnable = false;
                serialPort.RtsEnable = false;
                serialPort.Open();
                protocol.Reset();
                readerRunning = true;
                readerThread = new Thread(ReadSerialLoop);
                readerThread.IsBackground = true;
                readerThread.Name = "TingguSerialReader";
                readerThread.Start();
                SetConnectedState(true);
                AppendLog("已连接 " + serialPort.PortName + " @ 460800。正在等待设备状态……", Color.DarkGreen);
                SendCommand("STATUS");
            }
            catch (Exception ex)
            {
                AppendLog("连接失败：" + ex.Message, Color.Firebrick);
                Disconnect();
            }
        }

        private void Disconnect()
        {
            readerRunning = false;
            SerialPort port = serialPort;
            serialPort = null;
            if (port != null)
            {
                try { if (port.IsOpen) port.Close(); }
                catch { }
                try { port.Dispose(); }
                catch { }
            }
            if (readerThread != null && readerThread != Thread.CurrentThread)
            {
                try { readerThread.Join(600); }
                catch { }
            }
            readerThread = null;
            if (!IsDisposed) SetConnectedState(false);
        }

        private void ReadSerialLoop()
        {
            while (readerRunning)
            {
                SerialPort port = serialPort;
                if (port == null || !port.IsOpen) break;
                try
                {
                    string line = port.ReadLine();
                    protocol.AcceptLine(line);
                }
                catch (TimeoutException) { }
                catch (Exception ex)
                {
                    if (readerRunning) Ui(delegate { AppendLog("串口读取中断：" + ex.Message, Color.Firebrick); });
                    break;
                }
            }
        }

        private void SendCommand(string command)
        {
            lock (serialLock)
            {
                if (!IsConnected)
                {
                    AppendLog("尚未连接，不能发送 " + command + "。", Color.DarkOrange);
                    return;
                }
                try
                {
                    serialPort.WriteLine(command);
                    AppendLog("> " + command, Color.RoyalBlue);
                }
                catch (Exception ex)
                {
                    AppendLog("发送失败：" + ex.Message, Color.Firebrick);
                }
            }
        }

        private void HandleStatusLine(string line)
        {
            if (line.StartsWith("#STATE,"))
            {
                string state = line.Substring(7);
                stateLabel.Text = "状态：" + StateInChinese(state);
                stateLabel.ForeColor = state == "ARMED" ? Color.DarkGreen : (state == "CAPTURING" ? Color.DarkOrange : Color.Black);
                armButton.Enabled = IsConnected && state != "CAPTURING" && state != "TRANSFERRING";
                captureButton.Enabled = IsConnected && state == "ARMED";
            }
            else if (line.StartsWith("#MPU,FOUND"))
            {
                string[] fields = line.Split(',');
                mpuLabel.Text = "MPU：已连接 " + (fields.Length > 2 ? fields[2] : "");
                mpuLabel.ForeColor = Color.DarkGreen;
            }
            else if (line.StartsWith("#MPU_NOT_FOUND"))
            {
                mpuLabel.Text = "MPU：未找到（仍可采压电）";
                mpuLabel.ForeColor = Color.Firebrick;
            }
            else if (line.StartsWith("#STATUS,"))
            {
                string statusState = ExtractStatusText(line, "state=");
                if (statusState.Length > 0)
                {
                    stateLabel.Text = "状态：" + StateInChinese(statusState);
                    stateLabel.ForeColor = statusState == "ARMED" ? Color.DarkGreen : Color.Black;
                    captureButton.Enabled = IsConnected && statusState == "ARMED";
                }
                string address = ExtractStatusText(line, "address=");
                if (line.Contains("mpu=1"))
                {
                    mpuLabel.Text = "MPU：已连接 " + address;
                    mpuLabel.ForeColor = Color.DarkGreen;
                }
                else
                {
                    mpuLabel.Text = "MPU：未找到（仍可采压电）";
                    mpuLabel.ForeColor = Color.Firebrick;
                }
                int piezoRate = ExtractStatusInt(line, "piezo_rate=");
                int mpuRate = ExtractStatusInt(line, "mpu_rate=");
                int missed = ExtractStatusInt(line, "missed_total=");
                if (piezoRate > 0) samplingLabel.Text = "采样：压电" + piezoRate + " / MPU" + mpuRate + " Hz；累计错过" + missed;
            }
            else if (line.StartsWith("#CALIBRATING"))
            {
                stateLabel.Text = "状态：标定中，请保持铁板静止";
                stateLabel.ForeColor = Color.DarkOrange;
            }
            else if (line.StartsWith("#CALIBRATION,"))
            {
                AppendLog("标定完成：" + line.Substring(13), Color.DarkGreen);
                return;
            }
            else if (line.StartsWith("#ERROR"))
            {
                AppendLog(line, Color.Firebrick);
                return;
            }

            if (line.StartsWith("#HELLO") || line.StartsWith("#READY") || line.StartsWith("#TRIGGER") ||
                line.StartsWith("#ACK") || line.StartsWith("#MPU") || line.StartsWith("#STATE"))
                AppendLog(line, line.StartsWith("#ERROR") ? Color.Firebrick : Color.DimGray);
            else if (!line.StartsWith("正在接收事件"))
                AppendLog(line, Color.DimGray);
        }

        private void HandleCompletedEvent(EventData data)
        {
            try
            {
                AnalysisResult analysis = SignalAnalyzer.Analyze(data, (double)piezoSnrBox.Value, (double)mpuSnrBox.Value);
                ExperimentMetadata metadata = ReadMetadata();
                string csvPath = exporter.Export(data, analysis, metadata);
                CompletedEvent item = new CompletedEvent();
                item.Event = data;
                item.Analysis = analysis;
                item.Metadata = metadata;
                item.CsvPath = csvPath;
                history.Add(item);
                while (history.Count > 5) history.RemoveAt(0);

                RenderCharts();
                ShowMetrics(item);
                AppendLog("事件 " + data.EventId + " 接收完成：" + data.IntegrityMessage + "；已保存 " + csvPath, Color.DarkGreen);
            }
            catch (Exception ex)
            {
                verdictLabel.Text = "分析失败：" + ex.Message;
                verdictLabel.ForeColor = Color.Firebrick;
                AppendLog("事件分析/保存失败：" + ex, Color.Firebrick);
            }
        }

        private ExperimentMetadata ReadMetadata()
        {
            ExperimentMetadata metadata = new ExperimentMetadata();
            metadata.SpecimenId = specimenBox.Text.Trim();
            metadata.TightnessLabel = tightnessBox.Text.Trim();
            metadata.Torque = torqueBox.Text.Trim();
            metadata.StrikeMethod = strikeBox.Text.Trim();
            metadata.Notes = notesBox.Text.Trim();
            return metadata;
        }

        private void RenderCharts()
        {
            if (history.Count == 0) return;
            piezoChart.Series.Clear();
            mpuChart.Series.Clear();
            piezoChart.ChartAreas[0].AxisX.StripLines.Clear();
            mpuChart.ChartAreas[0].AxisX.StripLines.Clear();

            IEnumerable<CompletedEvent> items = overlayBox.Checked ? history : history.Skip(history.Count - 1);
            CompletedEvent latest = history[history.Count - 1];
            int itemIndex = 0;
            int itemCount = items.Count();
            foreach (CompletedEvent item in items)
            {
                bool current = itemIndex == itemCount - 1;
                Color color = current ? Color.RoyalBlue : Color.FromArgb(65 + itemIndex * 25, 100, 100, 100);
                Series piezo = NewLineSeries("压电 " + item.Event.EventId, color, current ? 2 : 1);
                foreach (SampleData sample in item.Event.Samples)
                    piezo.Points.AddXY(sample.TimeUs / 1000.0, sample.PiezoRaw - item.Analysis.PiezoBaseline);
                piezoChart.Series.Add(piezo);

                Series mpu = NewLineSeries("MPU " + item.Event.EventId + " " + AxisForDisplay(item),
                                           current ? Color.DarkOrange : Color.FromArgb(65 + itemIndex * 25, 180, 100, 40), current ? 2 : 1);
                foreach (SampleData sample in item.Event.Samples)
                {
                    if (!sample.MpuValid) continue;
                    mpu.Points.AddXY(sample.TimeUs / 1000.0, MpuDisplayValue(item, sample));
                }
                mpuChart.Series.Add(mpu);
                ++itemIndex;
            }

            double piezoLimit = Math.Max(latest.Analysis.PiezoAbsolutePeak * 1.15, latest.Event.TriggerThreshold * 1.4);
            AddHorizontalLine(piezoChart, "触发阈值 +", latest.Event.TriggerThreshold, Color.Firebrick);
            AddHorizontalLine(piezoChart, "触发阈值 -", -latest.Event.TriggerThreshold, Color.Firebrick);
            AddVerticalLine(piezoChart, "t=0", -piezoLimit, piezoLimit, Color.Black);

            double mpuLimit = Math.Max(0.01, latest.Analysis.MpuAbsolutePeakG * 1.2);
            AddVerticalLine(mpuChart, "t=0", -mpuLimit, mpuLimit, Color.Black);
            ShadeDuration(piezoChart, latest.Analysis.PiezoDurationMs, Color.FromArgb(28, Color.RoyalBlue));
            ShadeDuration(mpuChart, latest.Analysis.MpuDurationMs, Color.FromArgb(28, Color.DarkOrange));
            piezoChart.ChartAreas[0].RecalculateAxesScale();
            mpuChart.ChartAreas[0].RecalculateAxesScale();
        }

        private double MpuDisplayValue(CompletedEvent item, SampleData sample)
        {
            double x = (sample.Ax - item.Analysis.MpuMeanAxRaw) / item.Event.AccelLsbPerG;
            double y = (sample.Ay - item.Analysis.MpuMeanAyRaw) / item.Event.AccelLsbPerG;
            double z = (sample.Az - item.Analysis.MpuMeanAzRaw) / item.Event.AccelLsbPerG;
            string selected = mpuAxisBox.SelectedItem == null ? "主振动轴" : mpuAxisBox.SelectedItem.ToString();
            if (selected == "X") return x;
            if (selected == "Y") return y;
            if (selected == "Z") return z;
            if (selected == "动态合量") return Math.Sqrt(x * x + y * y + z * z);
            if (item.Analysis.DominantAxis == "X") return x;
            if (item.Analysis.DominantAxis == "Y") return y;
            return z;
        }

        private string AxisForDisplay(CompletedEvent item)
        {
            string selected = mpuAxisBox.SelectedItem == null ? "主振动轴" : mpuAxisBox.SelectedItem.ToString();
            return selected == "主振动轴" ? item.Analysis.DominantAxis : selected;
        }

        private void ShowMetrics(CompletedEvent item)
        {
            AnalysisResult r = item.Analysis;
            verdictLabel.Text = r.QualityText + "（这里只评价信号，不评价松紧）";
            verdictLabel.ForeColor = r.DualSensorValid ? Color.DarkGreen : Color.Firebrick;
            metricsGrid.Rows.Clear();
            AddMetric("压电峰值", r.PiezoAbsolutePeak.ToString("F1") + " counts", "压电SNR", FormatDb(r.PiezoSnrDb));
            AddMetric("压电持续时间", DurationText(r.PiezoDurationMs, r.PiezoDurationTruncated), "压电饱和", YesNo(r.PiezoSaturated));
            AddMetric("MPU主轴峰值", r.MpuAbsolutePeakG.ToString("F5") + " g", "MPU合量峰值", r.MpuVectorPeakG.ToString("F5") + " g");
            AddMetric("MPU SNR", FormatDb(r.MpuSnrDb), "MPU持续时间", DurationText(r.MpuDurationMs, r.MpuDurationTruncated));
            AddMetric("主振动轴", r.DominantAxis, "主频 / 周期", r.DominantFrequencyHz.ToString("F2") + " Hz / " + r.PeriodMs.ToString("F2") + " ms");
            AddMetric("丢点率", r.DropRatePercent.ToString("F3") + "%", "传输校验", item.Event.IntegrityMessage);
        }

        private void AddMetric(string name1, string value1, string name2, string value2)
        {
            metricsGrid.Rows.Add(name1, value1, name2, value2);
        }

        private void SetConnectedState(bool connected)
        {
            connectButton.Text = connected ? "断开" : "连接";
            portBox.Enabled = !connected;
            refreshButton.Enabled = !connected;
            recalibrateButton.Enabled = connected;
            armButton.Enabled = connected;
            captureButton.Enabled = false;
            if (!connected)
            {
                stateLabel.Text = "状态：未连接";
                stateLabel.ForeColor = Color.Black;
                mpuLabel.Text = "MPU：未知";
                mpuLabel.ForeColor = Color.Black;
            }
        }

        private void AppendLog(string text, Color color)
        {
            if (logBox.IsDisposed) return;
            logBox.AppendText("[" + DateTime.Now.ToString("HH:mm:ss") + "] " + text + Environment.NewLine);
            logBox.SelectionStart = logBox.TextLength;
            logBox.ScrollToCaret();
        }

        private void Ui(MethodInvoker action)
        {
            if (IsDisposed || !IsHandleCreated) return;
            try { BeginInvoke(action); }
            catch (InvalidOperationException) { }
        }

        private static Chart CreateChart(string title, string yTitle)
        {
            Chart chart = new Chart();
            chart.Dock = DockStyle.Fill;
            chart.BackColor = Color.White;
            ChartArea area = new ChartArea("main");
            area.AxisX.Title = "相对触发时间 (ms)，双击恢复缩放";
            area.AxisY.Title = yTitle;
            area.AxisX.MajorGrid.LineColor = Color.Gainsboro;
            area.AxisY.MajorGrid.LineColor = Color.Gainsboro;
            area.CursorX.IsUserEnabled = true;
            area.CursorX.IsUserSelectionEnabled = true;
            area.CursorY.IsUserEnabled = true;
            area.CursorY.IsUserSelectionEnabled = true;
            area.AxisX.ScaleView.Zoomable = true;
            area.AxisY.ScaleView.Zoomable = true;
            chart.ChartAreas.Add(area);
            chart.Titles.Add(title);
            chart.Legends.Add(new Legend("legend"));
            chart.Legends[0].Docking = Docking.Top;
            return chart;
        }

        private static Series NewLineSeries(string name, Color color, int width)
        {
            Series series = new Series(name);
            series.ChartType = SeriesChartType.FastLine;
            series.Color = color;
            series.BorderWidth = width;
            series.XValueType = ChartValueType.Double;
            series.YValueType = ChartValueType.Double;
            return series;
        }

        private static void AddHorizontalLine(Chart chart, string name, double y, Color color)
        {
            Series series = NewLineSeries(name, color, 1);
            series.BorderDashStyle = ChartDashStyle.Dash;
            series.Points.AddXY(-100.0, y);
            series.Points.AddXY(900.0, y);
            chart.Series.Add(series);
        }

        private static void AddVerticalLine(Chart chart, string name, double low, double high, Color color)
        {
            Series series = NewLineSeries(name, color, 1);
            series.BorderDashStyle = ChartDashStyle.Dash;
            series.Points.AddXY(0.0, low);
            series.Points.AddXY(0.0, high);
            chart.Series.Add(series);
        }

        private static void ShadeDuration(Chart chart, double durationMs, Color color)
        {
            if (durationMs <= 0.0) return;
            StripLine strip = new StripLine();
            strip.IntervalOffset = 0.0;
            strip.StripWidth = durationMs;
            strip.BackColor = color;
            chart.ChartAreas[0].AxisX.StripLines.Add(strip);
        }

        private static void ResetZoom(Chart chart)
        {
            chart.ChartAreas[0].AxisX.ScaleView.ZoomReset(0);
            chart.ChartAreas[0].AxisY.ScaleView.ZoomReset(0);
        }

        private static Label MakeLabel(string text, int width)
        {
            Label label = new Label();
            label.Text = text;
            label.Width = width;
            label.TextAlign = ContentAlignment.MiddleLeft;
            label.Margin = new Padding(3, 7, 3, 3);
            return label;
        }

        private static void SetButton(Button button, string text, int width)
        {
            button.Text = text;
            button.Width = width;
            button.Height = 28;
            button.Margin = new Padding(3, 1, 3, 3);
        }

        private static void AddLabeledControl(FlowLayoutPanel panel, string label, Control control, int width)
        {
            panel.Controls.Add(MakeLabel(label, TextRenderer.MeasureText(label, panel.Font).Width + 8));
            control.Width = width;
            control.Height = 25;
            control.Margin = new Padding(1, 2, 8, 3);
            panel.Controls.Add(control);
        }

        private static void ConfigureSnrBox(NumericUpDown control, decimal value)
        {
            control.Minimum = -20;
            control.Maximum = 60;
            control.DecimalPlaces = 1;
            control.Increment = 1;
            control.Value = value;
        }

        private void ConfigureMetricsGrid()
        {
            metricsGrid.Dock = DockStyle.Fill;
            metricsGrid.ReadOnly = true;
            metricsGrid.AllowUserToAddRows = false;
            metricsGrid.AllowUserToDeleteRows = false;
            metricsGrid.AllowUserToResizeRows = false;
            metricsGrid.RowHeadersVisible = false;
            metricsGrid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill;
            metricsGrid.BackgroundColor = Color.White;
            metricsGrid.BorderStyle = BorderStyle.None;
            metricsGrid.Columns.Add("metric1", "指标");
            metricsGrid.Columns.Add("value1", "结果");
            metricsGrid.Columns.Add("metric2", "指标");
            metricsGrid.Columns.Add("value2", "结果");
        }

        private static string StateInChinese(string state)
        {
            if (state == "IDLE") return "空闲/需ARM";
            if (state == "ARMING") return "正在积累敲击前数据，请稍等";
            if (state == "ARMED") return "已布防，可以敲击";
            if (state == "CAPTURING") return "正在记录本次敲击";
            if (state == "TRANSFERRING") return "正在传输，请勿操作";
            if (state == "HOLDING") return "本次完成；看完后点下一次采集";
            return state;
        }

        private static int ExtractStatusInt(string line, string key)
        {
            int start = line.IndexOf(key, StringComparison.Ordinal);
            if (start < 0) return 0;
            start += key.Length;
            int end = line.IndexOf(',', start);
            if (end < 0) end = line.Length;
            int value;
            return Int32.TryParse(line.Substring(start, end - start), NumberStyles.Integer, CultureInfo.InvariantCulture, out value) ? value : 0;
        }

        private static string ExtractStatusText(string line, string key)
        {
            int start = line.IndexOf(key, StringComparison.Ordinal);
            if (start < 0) return "";
            start += key.Length;
            int end = line.IndexOf(',', start);
            if (end < 0) end = line.Length;
            return line.Substring(start, end - start);
        }

        private static string DurationText(double value, bool truncated)
        {
            return value.ToString("F1") + " ms" + (truncated ? "（窗口截断）" : "");
        }

        private static string FormatDb(double value)
        {
            return Double.IsInfinity(value) || Double.IsNaN(value) ? "无有效SNR" : value.ToString("F2") + " dB";
        }

        private static string YesNo(bool value)
        {
            return value ? "是" : "否";
        }
    }
}
