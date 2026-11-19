using System;
using System.Windows.Controls;
using System.Windows.Input;

namespace OptScalerNR.Launcher
{
    // A checked parent (Small/Original) is required by a checked child
    // (Experimental/INT4). The card keeps its normal checked appearance but
    // must not be cleared by mouse, keyboard or a programmatic Unchecked.
    internal static class ModelDependencyLock
    {
        public const string TagValue = "dependencyLocked";

        public static bool IsLocked(CheckBox check)
        {
            return check != null && string.Equals(check.Tag as string, TagValue, StringComparison.Ordinal);
        }

        public static void Attach(CheckBox check, Func<bool> syncing,
                                  Action onUnlockedUncheck, Action<bool> setSyncing)
        {
            check.Unchecked += delegate {
                if (syncing()) return;
                if (!IsLocked(check)) {
                    onUnlockedUncheck();
                    return;
                }
                // Re-check synchronously: this is the final guard for keyboard,
                // mouse and accessibility TogglePattern paths.
                setSyncing(true);
                try { check.IsChecked = true; }
                finally { setSyncing(false); }
            };
            check.PreviewMouseLeftButtonDown += delegate(object sender, MouseButtonEventArgs e) {
                if (IsLocked(check)) e.Handled = true;
            };
            check.PreviewKeyDown += delegate(object sender, KeyEventArgs e) {
                if (IsLocked(check) && (e.Key == Key.Space || e.Key == Key.Enter)) e.Handled = true;
            };
        }
    }
}
