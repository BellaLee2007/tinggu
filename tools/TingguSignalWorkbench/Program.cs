using System;
using System.Windows.Forms;

namespace TingguSignalWorkbench
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Length > 0 && args[0] == "--self-test")
            {
                try
                {
                    SelfTest.Run();
                    Console.WriteLine("SELF_TEST_OK");
                    return 0;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("SELF_TEST_FAILED: " + ex);
                    return 1;
                }
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }
}
