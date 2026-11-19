using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Documents;

namespace OptScalerNR.Launcher
{
    public static class DialogTheme
    {
        public static readonly DependencyProperty KeyboardFocusVisibleProperty = DependencyProperty.RegisterAttached("KeyboardFocusVisible", typeof(bool), typeof(DialogTheme), new FrameworkPropertyMetadata(false));
        public static bool GetKeyboardFocusVisible(DependencyObject target) { return (bool)target.GetValue(KeyboardFocusVisibleProperty); }
        public static void SetKeyboardFocusVisible(DependencyObject target, bool value) { target.SetValue(KeyboardFocusVisibleProperty, value); }
        static bool keyboardNavigation;
        static DialogTheme()
        {
            EventManager.RegisterClassHandler(typeof(Window), Keyboard.PreviewKeyDownEvent, new KeyEventHandler(delegate(object sender, KeyEventArgs e) {
                if (e.Key == Key.Tab || e.Key == Key.Left || e.Key == Key.Right || e.Key == Key.Up || e.Key == Key.Down) keyboardNavigation = true;
            }), true);
            EventManager.RegisterClassHandler(typeof(Window), Mouse.PreviewMouseDownEvent, new MouseButtonEventHandler(delegate {
                keyboardNavigation = false;
                var focused = Keyboard.FocusedElement as DependencyObject;
                if (focused != null) SetKeyboardFocusVisible(focused, false);
            }), true);
            EventManager.RegisterClassHandler(typeof(Control), Keyboard.GotKeyboardFocusEvent, new KeyboardFocusChangedEventHandler(delegate(object sender, KeyboardFocusChangedEventArgs e) {
                if (ReferenceEquals(sender, e.NewFocus)) SetKeyboardFocusVisible((DependencyObject)sender, keyboardNavigation);
            }), true);
            EventManager.RegisterClassHandler(typeof(Control), Keyboard.LostKeyboardFocusEvent, new KeyboardFocusChangedEventHandler(delegate(object sender, KeyboardFocusChangedEventArgs e) {
                if (ReferenceEquals(sender, e.OldFocus)) SetKeyboardFocusVisible((DependencyObject)sender, false);
            }), true);
        }
        public static ResourceDictionary Resources()
        {
            ResourceDictionary resources;
            using (var stream = Program.OpenXaml("DialogTheme.xaml")) resources = (ResourceDictionary)XamlReader.Load(stream);
            resources.MergedDictionaries.Add(LauncherTheme.Resources);
            return resources;
        }
        public static Window Create(Window owner, string title, double width)
        {
            var window = new Window { Title = title, Width = width, SizeToContent = SizeToContent.Height };
            Apply(window, owner);
            return window;
        }
        public static void Apply(Window window, Window owner)
        {
            window.Owner = owner != null && owner.IsVisible ? owner : null;
            window.WindowStartupLocation = window.Owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen;
            window.ShowInTaskbar = false;
            window.ResizeMode = ResizeMode.NoResize;
            window.WindowStyle = WindowStyle.None;
            // Fixed-size dialogs draw their entire frame in WPF and drag via
            // the header's DragMove handler. WindowChrome is only needed by
            // the resizable main window; do not add its native frame here.
            // Match the main window: SizeToContent must land on physical pixels too.
            window.UseLayoutRounding = true;
            window.SnapsToDevicePixels = true;
            window.SetResourceReference(Window.BackgroundProperty, "ThemeSurface");
            window.SetResourceReference(Window.ForegroundProperty, "ThemeInk");
            window.SetResourceReference(Window.BorderBrushProperty, "ThemeBorder");
            window.BorderThickness = new Thickness(1);
            // At fractional DPI the default Window Border can leave a transparent
            // scanline between its background and bottom stroke. Paint an opaque
            // backing independently of that Border's inner geometry.
            var template = new ControlTemplate(typeof(Window));
            var backing = new FrameworkElementFactory(typeof(Grid));
            backing.SetValue(Panel.BackgroundProperty, new TemplateBindingExtension(Window.BackgroundProperty));
            var frame = new FrameworkElementFactory(typeof(Border));
            frame.SetValue(Border.BorderBrushProperty, new TemplateBindingExtension(Window.BorderBrushProperty));
            frame.SetValue(Border.BorderThicknessProperty, new TemplateBindingExtension(Window.BorderThicknessProperty));
            var adorners = new FrameworkElementFactory(typeof(AdornerDecorator));
            var presenter = new FrameworkElementFactory(typeof(ContentPresenter));
            presenter.SetValue(ContentPresenter.ContentProperty, new TemplateBindingExtension(Window.ContentProperty));
            presenter.SetValue(ContentPresenter.ContentTemplateProperty, new TemplateBindingExtension(Window.ContentTemplateProperty));
            presenter.SetValue(ContentPresenter.ContentStringFormatProperty, new TemplateBindingExtension(Window.ContentStringFormatProperty));
            adorners.AppendChild(presenter);
            frame.AppendChild(adorners);
            backing.AppendChild(frame);
            template.VisualTree = backing;
            window.Template = template;
            window.FontFamily = owner != null ? owner.FontFamily : new FontFamily("Segoe UI, Microsoft YaHei UI");
            window.FontSize = 13;
            window.Language = owner != null ? owner.Language : XmlLanguage.GetLanguage("en-US");
            window.MaxHeight = Math.Max(300, Math.Min(800, SystemParameters.WorkArea.Height - 50));
            window.MaxWidth = Math.Max(300, SystemParameters.WorkArea.Width - 40);
            window.Resources.MergedDictionaries.Add(Resources());
            window.PreviewKeyDown += delegate(object sender, KeyEventArgs e) { if(e.Key == Key.Escape) { window.Close(); e.Handled = true; } };
        }
        public static void Primary(Window window, Button button)
        {
            button.Style = window.FindResource("DialogPrimary") as Style;
            button.IsDefault = true;
        }
        public static void SetContent(Window window, UIElement content)
        {
            var root = new DockPanel();
            root.SetResourceReference(DockPanel.BackgroundProperty, "ThemeSurface");
            var header = new Border { Padding = new Thickness(20,12,12,12), BorderThickness = new Thickness(0,0,0,1) };
            header.SetResourceReference(Border.BorderBrushProperty, "ThemeBorder");
            var row = new DockPanel();
            var close = new Button { Content = "✕", Padding = new Thickness(10,4,10,4), MinHeight = 30, ToolTip = window.Language.IetfLanguageTag.StartsWith("zh") ? "关闭" : "Close" };
            close.Click += delegate { window.Close(); };
            DockPanel.SetDock(close, Dock.Right); row.Children.Add(close);
            row.Children.Add(new TextBlock { Text = window.Title, FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            header.Child = row;
            header.MouseLeftButtonDown += delegate(object sender, MouseButtonEventArgs e) { if (e.ButtonState == MouseButtonState.Pressed) { try { window.DragMove(); } catch { } } };
            DockPanel.SetDock(header, Dock.Top); root.Children.Add(header);
            var viewer = content as ScrollViewer;
            if (viewer == null) viewer = new ScrollViewer { Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
            viewer.MaxHeight = window.MaxHeight - 64;
            root.Children.Add(viewer);
            window.Content = root;
        }
        public static void SetContent(Window window, UIElement content, UIElement footer)
        {
            var root = new DockPanel();
            root.SetResourceReference(DockPanel.BackgroundProperty, "ThemeSurface");
            var header = new Border { Padding = new Thickness(20,12,12,12), BorderThickness = new Thickness(0,0,0,1) };
            header.SetResourceReference(Border.BorderBrushProperty, "ThemeBorder");
            var row = new DockPanel();
            var close = new Button { Content = "✕", Padding = new Thickness(10,4,10,4), MinHeight = 30, ToolTip = window.Language.IetfLanguageTag.StartsWith("zh") ? "关闭" : "Close" };
            close.Click += delegate { window.Close(); };
            DockPanel.SetDock(close, Dock.Right); row.Children.Add(close);
            row.Children.Add(new TextBlock { Text = window.Title, FontSize = 15, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            header.Child = row;
            header.MouseLeftButtonDown += delegate(object sender, MouseButtonEventArgs e) { if (e.ButtonState == MouseButtonState.Pressed) { try { window.DragMove(); } catch { } } };
            DockPanel.SetDock(header, Dock.Top); root.Children.Add(header);
            var footerBorder = new Border { BorderThickness = new Thickness(0,1,0,0), Padding = new Thickness(20,10,20,14) };
            footerBorder.SetResourceReference(Border.BorderBrushProperty, "ThemeBorder");
            footerBorder.Child = footer;
            DockPanel.SetDock(footerBorder, Dock.Bottom); root.Children.Add(footerBorder);
            var viewer = content as ScrollViewer;
            if (viewer == null) viewer = new ScrollViewer { Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
            viewer.MaxHeight = window.MaxHeight - 64;
            root.Children.Add(viewer);
            window.Content = root;
        }
        public static MessageBoxResult Show(Window owner, string text, string caption)
        { return Show(owner, text, caption, MessageBoxButton.OK, MessageBoxImage.Information); }
        public static MessageBoxResult Show(Window owner, string text, string caption, MessageBoxButton buttons)
        { return Show(owner, text, caption, buttons, MessageBoxImage.Information); }
        public static MessageBoxResult Show(Window owner, string text, string caption, MessageBoxButton buttons, MessageBoxImage image)
        {
            bool zh = owner != null && owner.Language.IetfLanguageTag.StartsWith("zh");
            var window = Create(owner, caption, 600);
            var panel = new StackPanel { Margin = new Thickness(22) };
            var message = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0,0,0,20) };
            message.SetResourceReference(TextBlock.ForegroundProperty, image == MessageBoxImage.Error ? "ThemeWarning" : "ThemeInk");
            panel.Children.Add(message);
            var row = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Right };
            var result = MessageBoxResult.Cancel;
            Action<string,MessageBoxResult,bool> add = (label, value, primary) => {
                var b = new Button { Content = label, MinWidth = 90, Margin = new Thickness(4) };
                if (primary) Primary(window, b);
                b.Click += delegate { result = value; window.DialogResult = true; };
                row.Children.Add(b);
            };
            if (buttons == MessageBoxButton.YesNo || buttons == MessageBoxButton.YesNoCancel) {
                if (buttons == MessageBoxButton.YesNoCancel) add(zh ? "取消" : "Cancel", MessageBoxResult.Cancel, false);
                add(zh ? "否" : "No", MessageBoxResult.No, false);
                add(zh ? "是" : "Yes", MessageBoxResult.Yes, true);
            } else {
                if (buttons == MessageBoxButton.OKCancel) add(zh ? "取消" : "Cancel", MessageBoxResult.Cancel, false);
                add(zh ? "确定" : "OK", MessageBoxResult.OK, true);
            }
            panel.Children.Add(row); SetContent(window, panel);
            window.ShowDialog();
            return result;
        }
    }
}
