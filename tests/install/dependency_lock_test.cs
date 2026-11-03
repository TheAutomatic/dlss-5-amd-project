using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using OptScalerNR.Launcher;

static class DependencyLockTest
{
    static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    [STAThread]
    static void Main()
    {
        var check = new CheckBox { Tag = ModelDependencyLock.TagValue, IsChecked = true };
        bool syncing = false;
        int changed = 0;
        ModelDependencyLock.Attach(check, () => syncing,
            delegate { ++changed; }, value => syncing = value);

        // Real WPF Unchecked route: a locked parent must return to checked.
        check.IsChecked = false;
        Check(check.IsChecked == true, "locked parent was cleared");
        Check(changed == 0, "locked parent reported a user change");

        // The mouse guard marks the press handled; the checkbox remains checked.
        var mouse = new MouseButtonEventArgs(Mouse.PrimaryDevice, 0, MouseButton.Left) {
            RoutedEvent = UIElement.PreviewMouseLeftButtonDownEvent
        };
        check.RaiseEvent(mouse);
        Check(mouse.Handled, "locked mouse press was not handled");
        Check(check.IsChecked == true, "locked mouse press cleared the parent");

        // Removing the dependency lock restores normal cancellation.
        check.Tag = null;
        check.IsChecked = false;
        Check(check.IsChecked == false, "unlocked parent did not clear");
        Check(changed == 1, "unlocked uncheck callback count: " + changed);
        Console.WriteLine("PASS dependency lock");
    }
}
