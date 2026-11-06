using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        Button _addFolderButton, _manageGameButton;
        ContextMenu _manageGameMenu;
        MenuItem _changeExeMenuItem, _scanExeMenuItem, _editGameMenuItem, _removeSelectedGameMenuItem, _launchSteamMenuItem;
        CancellationTokenSource _folderScan;
        static string EntryLocation(GameInfo game) { return game == null ? null : !string.IsNullOrEmpty(game.Exe) ? game.Exe : game.RootDirectory; }
        static bool SamePath(string a,string b)
        {
            if(string.IsNullOrWhiteSpace(a)||string.IsNullOrWhiteSpace(b))return false;
            try {return string.Equals(Path.GetFullPath(a).TrimEnd('\\','/'),Path.GetFullPath(b).TrimEnd('\\','/'),StringComparison.OrdinalIgnoreCase);}catch{return false;}
        }
        void InitLibraryActions()
        {
            _addFolderButton=(Button)_window.FindName("AddFolderButton");
            _manageGameButton=(Button)_window.FindName("ManageGameButton");
            if(_addFolderButton!=null)_addFolderButton.Click+=(s,e)=>AddFolder();
            if(_manageGameButton!=null)
            {
                var itemStyle=(Style)_window.FindResource("GameContextMenuItem");
                _manageGameMenu=new ContextMenu{Style=(Style)_window.FindResource("GameContextMenu")};
                _launchSteamMenuItem=new MenuItem{Style=itemStyle};_launchSteamMenuItem.Click+=(s,e)=>LaunchSelectedGameViaSteam();
                _changeExeMenuItem=new MenuItem{Style=itemStyle};_changeExeMenuItem.Click+=(s,e)=>ChangeExe();
                _scanExeMenuItem=new MenuItem{Style=itemStyle};_scanExeMenuItem.Click+=(s,e)=>ScanExecutables();
                _editGameMenuItem=new MenuItem{Style=itemStyle};_editGameMenuItem.Click+=(s,e)=>EditSelectedGame();
                _removeSelectedGameMenuItem=new MenuItem{Style=itemStyle};_removeSelectedGameMenuItem.Click+=(s,e)=>RemoveGame(_selectedGame);
                _manageGameMenu.Items.Add(_launchSteamMenuItem);
                _manageGameMenu.Items.Add(_changeExeMenuItem);
                _manageGameMenu.Items.Add(_scanExeMenuItem);
                _manageGameMenu.Items.Add(_editGameMenuItem);
                _manageGameMenu.Items.Add(new Separator{Style=(Style)_window.FindResource("GameContextMenuSeparator")});
                _manageGameMenu.Items.Add(_removeSelectedGameMenuItem);
                _manageGameButton.ContextMenu=_manageGameMenu;
                _manageGameButton.ContextMenuOpening+=(s,e)=>{
                    if(_folderScan!=null||_selectedGame==null){e.Handled=true;return;}
                    RefreshLibraryActions();
                };
                _manageGameButton.Click+=(s,e)=>{
                    if(_folderScan!=null){ScanExecutables();return;}
                    if(_selectedGame==null||_manageGameMenu==null)return;
                    RefreshLibraryActions();
                    _manageGameMenu.PlacementTarget=_manageGameButton;
                    _manageGameMenu.Placement=System.Windows.Controls.Primitives.PlacementMode.Bottom;
                    _manageGameMenu.IsOpen=true;
                };
            }
        }
        void RefreshLibraryActions()
        {
            bool busy=_writeBusy||(_session!=null&&_session.IsRunning&&(_presentationBusy||!_session.CanKill))||_folderScan!=null;
            bool canManage=_selectedGame!=null&&!busy;
            if(_addFolderButton!=null){_addFolderButton.Content=L("+ 添加文件夹","+ Add Folder");_addFolderButton.IsEnabled=!busy;}
            if(_manageGameButton!=null){
                _manageGameButton.Content=_folderScan!=null?L("取消扫描","Cancel scan"):L("管理 ▾","Manage ▾");
                _manageGameButton.IsEnabled=_folderScan!=null||canManage;
                if(!canManage&&_manageGameMenu!=null)_manageGameMenu.IsOpen=false;
            }
            bool isSteam = _selectedGame != null && !string.IsNullOrEmpty(_selectedGame.Id) && _selectedGame.Id.StartsWith("steam:", StringComparison.OrdinalIgnoreCase);
            if(_launchSteamMenuItem!=null){_launchSteamMenuItem.Header=L("通过 Steam 启动…","Launch via Steam…");_launchSteamMenuItem.Visibility=isSteam?Visibility.Visible:Visibility.Collapsed;_launchSteamMenuItem.IsEnabled=canManage;}
            if(_changeExeMenuItem!=null){_changeExeMenuItem.Header=L("更换 EXE…","Change EXE…");_changeExeMenuItem.IsEnabled=canManage;}
            if(_scanExeMenuItem!=null){_scanExeMenuItem.Header=L("扫描启动程序…","Find game EXE…");_scanExeMenuItem.IsEnabled=canManage;}
            if(_editGameMenuItem!=null){_editGameMenuItem.Header=L("编辑信息…","Edit details…");_editGameMenuItem.IsEnabled=canManage;}
            if(_removeSelectedGameMenuItem!=null){_removeSelectedGameMenuItem.Header=L("从列表移除","Remove from list");_removeSelectedGameMenuItem.IsEnabled=canManage;}
        }
        void LaunchSelectedGameViaSteam()
        {
            if (_selectedGame == null) return;
            try
            {
                var appId = _selectedGame.Id.StartsWith("steam:", StringComparison.OrdinalIgnoreCase)
                    ? _selectedGame.Id.Substring("steam:".Length).Trim()
                    : null;
                if (!string.IsNullOrEmpty(appId))
                {
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("steam://rungameid/" + appId) { UseShellExecute = true });
                    if (_statusText != null)
                        _statusText.Text = L("已请求 Steam 启动：" + _selectedGame.DisplayName, "Requested Steam to launch: " + _selectedGame.DisplayName);
                }
            }
            catch (Exception ex)
            {
                DialogTheme.Show(_window, L("无法通过 Steam 启动：" + ex.Message, "Failed to launch via Steam: " + ex.Message), L("启动失败", "Launch Failed"));
            }
        }

        void RestoreHiddenPath(string path)
        {
            foreach(var key in _hiddenGames.Where(p=>SamePath(p.Value,path)).Select(p=>p.Key).ToArray())_hiddenGames.Remove(key);
        }
        void AddFolder()
        {
            try {
                var root=FolderPicker.Pick(_window,L("选择游戏文件夹（可暂时没有 EXE）","Select game folder (an EXE is not required yet)"),_selectedGame==null?null:_selectedGame.RootDirectory);
                if(!string.IsNullOrEmpty(root))AddFolderFromPath(root);
            } catch(Exception ex){DialogTheme.Show(_window,ex.Message,_window.Title);}
        }
        void AddFolderFromPath(string path)
        {
            if(_writeBusy || !Directory.Exists(path))return;
            var root=Path.GetFullPath(path).TrimEnd('\\','/');
            if(root.EndsWith(":"))root+="\\";
            if((File.GetAttributes(root)&FileAttributes.ReparsePoint)!=0){DialogTheme.Show(_window,L("请选择实际文件夹，不使用目录链接。","Select the actual folder rather than a directory link."),_window.Title);return;}
            RestoreHiddenPath(root);
            var game=_allGames.Find(g=>SamePath(g.RootDirectory,root));
            if(game==null){game=new GameInfo{Id="manual:"+Guid.NewGuid().ToString("N"),Name=Path.GetFileName(root.TrimEnd('\\','/')),Root=root,Source="Manual"};if(string.IsNullOrEmpty(game.Name))game.Name=root;_allGames.Add(game);}
            SaveSettings();SaveLibraryCache();RefreshGameListView();SelectGame(game);
        }
        void BindExecutable(GameInfo game,string path)
        {
            var normal=GamePaths.Normalize(path);
            if(game==null||string.IsNullOrEmpty(normal)||!GamePaths.CanUse(normal))return;
            if(string.IsNullOrEmpty(game.Root))game.Root=Path.GetDirectoryName(normal);
            game.Exe=normal;game.ExeOverride=true;
            game.Installed=File.Exists(Path.Combine(Path.GetDirectoryName(normal),"amd-presr-install.txt"));
            RestoreHiddenPath(normal);
            if(!string.IsNullOrEmpty(game.Id))_hiddenGames.Remove(game.Id);
            // Keep the existing ID, including legacy custom IDs, and every display override.
            SaveSettings();SaveLibraryCache();RefreshGameListView();SelectGame(game);
        }
        static BitmapImage ReadCover(string path)
        {
            if(new FileInfo(path).Length>32*1024*1024)throw new IOException("Select an image smaller than 32 MiB.");
            var absolute=new Uri(Path.GetFullPath(path),UriKind.Absolute);
            int width,height;
            using(var input=File.OpenRead(path)){
                var frame=BitmapFrame.Create(input,BitmapCreateOptions.DelayCreation,BitmapCacheOption.None);
                width=frame.PixelWidth;height=frame.PixelHeight;
                if(width<1||height<1||width>32768||height>32768||(long)width*height>100000000)throw new IOException("Image dimensions are too large.");
            }
            var image=new BitmapImage();image.BeginInit();image.CacheOption=BitmapCacheOption.OnLoad;
            if(width>=height)image.DecodePixelWidth=Math.Min(width,1024);else image.DecodePixelHeight=Math.Min(height,1024);
            image.CreateOptions=BitmapCreateOptions.IgnoreImageCache;
            image.UriSource=absolute;image.EndInit();image.Freeze();return image;
        }
        void EditSelectedGame()
        {
            if(_selectedGame==null||_writeBusy||_folderScan!=null)return;
            var game=_selectedGame;
            var dialog=CreateGameDetailsDialog(game);
            if(dialog.ShowDialog()==true){RefreshGameListView();SelectGame(game);}
        }
        Window CreateGameDetailsDialog(GameInfo game)
        {
            var dialog=DialogTheme.Create(_window,L("编辑游戏信息","Edit game details"),560);
            var panel=new StackPanel{Margin=new Thickness(22)};
            panel.Children.Add(new TextBlock{Text=L("显示名称","Display name"),Margin=new Thickness(0,0,0,7)});
            var name=new TextBox{Text=game.DisplayName,MaxLength=100};panel.Children.Add(name);
            var resetName=new Button{Content=L("恢复默认名称","Reset name"),HorizontalAlignment=HorizontalAlignment.Left,Margin=new Thickness(0,8,0,16)};
            resetName.Click+=delegate{name.Text=game.Name??"";};panel.Children.Add(resetName);
            var preview=new Image{Height=190,Stretch=Stretch.Uniform,HorizontalAlignment=HorizontalAlignment.Left};panel.Children.Add(preview);
            string picked=null;bool resetCover=false;
            try{if(!string.IsNullOrEmpty(game.DisplayCover))preview.Source=ReadCover(game.DisplayCover);}catch{}
            var choose=new Button{Content=L("选择封面图片…","Choose cover image…"),Margin=new Thickness(0,10,8,12)};
            choose.Click+=delegate{
                var picker=new Microsoft.Win32.OpenFileDialog{Title=L("选择封面","Choose cover"),Filter="Images (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg",CheckFileExists=true};
                if(picker.ShowDialog(dialog)==true)try{preview.Source=ReadCover(picker.FileName);picked=picker.FileName;resetCover=false;}catch(Exception ex){DialogTheme.Show(dialog,ex.Message,dialog.Title);}
            };
            var reset=new Button{Content=L("恢复默认图片","Reset image"),Margin=new Thickness(0,10,0,12)};
            reset.Click+=delegate{picked=null;resetCover=true;preview.Source=null;try{if(!string.IsNullOrEmpty(game.Cover))preview.Source=ReadCover(game.Cover);}catch{}};
            var images=new WrapPanel();images.Children.Add(choose);images.Children.Add(reset);panel.Children.Add(images);
            panel.Children.Add(new TextBlock{Text=EntryLocation(game),TextWrapping=TextWrapping.Wrap,Foreground=LauncherTheme.Brush("ThemeMuted"),Margin=new Thickness(0,0,0,16)});
            var buttons=new WrapPanel{HorizontalAlignment=HorizontalAlignment.Right};
            buttons.Children.Add(new Button{Content=L("取消","Cancel"),IsCancel=true,Margin=new Thickness(4)});
            var save=new Button{Content=L("保存","Save"),Margin=new Thickness(4)};DialogTheme.Primary(dialog,save);
            save.Click+=delegate{
                if(string.IsNullOrWhiteSpace(name.Text))return;
                try{
                    string cover=resetCover?null:game.CustomCover;
                    if(picked!=null){
                        string key;using(var sha=SHA256.Create())key=BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(game.Id))).Replace("-","").ToLowerInvariant();
                        var dir=Path.Combine(_settingsDir,"user-assets",key);Directory.CreateDirectory(dir);
                        var output=Path.Combine(dir,Guid.NewGuid().ToString("N")+".png");
                        var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(ReadCover(picked)));
                        using(var stream=new FileStream(output,FileMode.CreateNew,FileAccess.Write))encoder.Save(stream);
                        cover=output;
                    }
                    game.CustomName=string.Equals(name.Text.Trim(),game.Name,StringComparison.Ordinal)?null:name.Text.Trim();game.CustomCover=cover;
                    SaveLibraryCache();dialog.DialogResult=true;
                }catch(Exception ex){DialogTheme.Show(dialog,ex.Message,dialog.Title);}
            };
            buttons.Children.Add(save);panel.Children.Add(buttons);DialogTheme.SetContent(dialog,panel);
            return dialog;
        }

        async void ScanExecutables()
        {
            if(_folderScan!=null){_folderScan.Cancel();return;}
            if(_selectedGame==null||_writeBusy||(_session!=null&&_session.IsRunning))return;
            var game=_selectedGame;var root=game.RootDirectory;
            if(string.IsNullOrEmpty(root)||!Directory.Exists(root)){
                root=FolderPicker.Pick(_window,L("选择要扫描的游戏文件夹","Select game folder to scan"),root);
                if(string.IsNullOrEmpty(root))return;game.Root=root;SaveLibraryCache();
            }
            var cancellation=new CancellationTokenSource();_folderScan=cancellation;
            SetBusy(true,"scan");RefreshLibraryActions();if(_progress!=null)_progress.IsIndeterminate=true;
            try{
                var result=await FolderGameScanner.ScanAsync(root,cancellation.Token,(dirs,exes)=>_window.Dispatcher.BeginInvoke(new Action(()=>{
                    if(ReferenceEquals(_folderScan,cancellation)&&_statusText!=null)_statusText.Text=L("扫描中：","Scanning: ")+dirs+L(" 个目录，"," folders, ")+exes+L(" 个程序"," executables");
                })));
                if(cancellation.IsCancellationRequested||!ReferenceEquals(_selectedGame,game))return;
                _folderScan=null;SetBusy(false,"");if(_progress!=null)_progress.IsIndeterminate=false;UpdatePrimaryButtonState();
                ChooseExecutable(game,result);
            }catch(Exception ex){if(!cancellation.IsCancellationRequested)DialogTheme.Show(_window,ex.Message,_window.Title);}
            finally{
                if(ReferenceEquals(_folderScan,cancellation)){_folderScan=null;SetBusy(false,"");if(_progress!=null)_progress.IsIndeterminate=false;UpdatePrimaryButtonState();}
                cancellation.Dispose();
            }
        }
        void ChooseExecutable(GameInfo game,FolderScanResult result)
        {
            var dialog=CreateExecutableDialog(result);
            if(dialog.ShowDialog()==true){var selected=(ExeCandidate)dialog.Tag;BindExecutable(game,selected.Path);}
        }
        Window CreateExecutableDialog(FolderScanResult result)
        {
            var dialog=DialogTheme.Create(_window,L("选择游戏启动程序","Choose game executable"),800);
            var panel=new StackPanel{Margin=new Thickness(22)};
            panel.Children.Add(new TextBlock{Text=(result.Partial?L("扫描已到限制，以下为部分结果。可缩小目录或再次扫描。\n","Scan reached a limit; these are partial results. Narrow the folder or scan again.\n"):"")+L("图形 API 是识别线索，不保证它是主程序或代理一定可用。请选择后确认。","Graphics API evidence is a clue, not proof of the rendering process or a working proxy. Review and confirm your choice."),TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,0,0,12)});
            var all=new CheckBox{Content=new TextBlock{Text=L("显示全部 EXE（包括未识别 API 和辅助程序）","Show all EXEs (including unknown APIs and helper programs)"),TextWrapping=TextWrapping.Wrap}};panel.Children.Add(all);
            var list=new ListBox{Height=310,Margin=new Thickness(0,10,0,12)};ScrollViewer.SetHorizontalScrollBarVisibility(list,ScrollBarVisibility.Disabled);panel.Children.Add(list);
            var use=new Button{Content=L("使用此程序","Use this program"),IsEnabled=false,Margin=new Thickness(4)};DialogTheme.Primary(dialog,use);
            var artwork = new LocalArtwork.Scope(_window.Dispatcher);
            dialog.Closed += delegate { artwork.Replace(null); };
            Action populate=()=>{
                artwork.Replace(null);
                var requests = new List<LocalArtwork.Request>();
                list.Items.Clear();
                foreach(var item in result.Candidates){
                    if(all.IsChecked!=true&&(item.Apis.Length==0||item.Auxiliary||item.Architecture!="x64"))continue;
                    var row=new StackPanel();
                    row.Children.Add(new TextBlock{Text=item.FileName+"  ·  "+item.Architecture+"  ·  "+(item.Apis.Length==0?L("API 未识别","API unknown"):string.Join(" / ",item.Apis)),FontWeight=FontWeights.SemiBold,TextWrapping=TextWrapping.Wrap});
                    row.Children.Add(new TextBlock{Text=item.RelativePath,TextWrapping=TextWrapping.Wrap,Foreground=LauncherTheme.Brush("ThemeMuted"),Margin=new Thickness(0,5,0,0)});
                    row.Children.Add(new TextBlock{Text=item.Auxiliary?L("辅助程序，通常不作为游戏入口","Helper program; usually not the game entry"):item.Evidence=="direct"?L("直接导入图形 API","Direct graphics API import"):item.Evidence=="delay"?L("延迟导入图形 API","Delayed graphics API import"):item.Evidence=="dependency"?L("本地依赖中发现图形 API","Graphics API found in a local dependency"):L("可能动态加载 API，请根据位置判断","May load APIs dynamically; review its location"),Foreground=LauncherTheme.Brush("ThemeMuted"),TextWrapping=TextWrapping.Wrap,Margin=new Thickness(0,4,0,0)});
                    var layout=new DockPanel();
                    var image=new Image{Width=28,Height=28,Margin=new Thickness(0,0,12,0),VerticalAlignment=VerticalAlignment.Top};DockPanel.SetDock(image,Dock.Left);layout.Children.Add(image);
                    requests.Add(new LocalArtwork.Request { Path=item.Path, Pixels=ArtworkPixels(28), Icon=true, Complete=icon=>image.Source=icon });
                    layout.Children.Add(row);
                    list.Items.Add(new ListBoxItem{Content=layout,Tag=item,ToolTip=item.Path});
                }
                artwork.Replace(requests);
                use.IsEnabled=false;
            };
            all.Checked+=(s,e)=>populate();all.Unchecked+=(s,e)=>populate();populate();
            if(list.Items.Count==0)all.IsChecked=true;
            if(result.Candidates.Length==0)panel.Children.Add(new TextBlock{Text=L("没有找到 EXE。文件夹条目会保留，可更换目录或稍后重试。","No EXE found. The folder entry is retained; choose another folder or try later."),TextWrapping=TextWrapping.Wrap});
            list.SelectionChanged+=(s,e)=>{var selected=list.SelectedItem as ListBoxItem;var exe=selected==null?null:selected.Tag as ExeCandidate;use.IsEnabled=exe!=null&&exe.Architecture=="x64";};
            var buttons=new WrapPanel{HorizontalAlignment=HorizontalAlignment.Right};
            var locate=new Button{Content=L("打开所选位置","Open selected location"),Margin=new Thickness(4)};
            locate.Click+=delegate{var selected=list.SelectedItem as ListBoxItem;if(selected!=null)System.Diagnostics.Process.Start("explorer.exe","/select,\""+((ExeCandidate)selected.Tag).Path+"\"");};
            buttons.Children.Add(locate);buttons.Children.Add(new Button{Content=L("取消","Cancel"),IsCancel=true,Margin=new Thickness(4)});buttons.Children.Add(use);
            use.Click+=delegate{dialog.Tag=((ListBoxItem)list.SelectedItem).Tag;dialog.DialogResult=true;};
            panel.Children.Add(buttons);DialogTheme.SetContent(dialog,panel);
            return dialog;
        }
    }

    static class FolderPicker
    {
        [ComImport,Guid("DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7")]class FileOpenDialog{}
        [ComImport,Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellItem {void BindToHandler(IntPtr p,ref Guid b,ref Guid i,out IntPtr o);void GetParent(out IShellItem p);void GetDisplayName(uint type,out IntPtr name);void GetAttributes(uint mask,out uint attrs);void Compare(IShellItem other,uint hint,out int order);}
        [ComImport,Guid("42F85136-DB7E-439C-85F1-E4075D135FC8"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IFileDialog {
            [PreserveSig]int Show(IntPtr owner);void SetFileTypes(uint count,IntPtr types);void SetFileTypeIndex(uint index);void GetFileTypeIndex(out uint index);void Advise(IntPtr events,out uint cookie);void Unadvise(uint cookie);void SetOptions(uint options);void GetOptions(out uint options);void SetDefaultFolder(IShellItem folder);void SetFolder(IShellItem folder);void GetFolder(out IShellItem folder);void GetCurrentSelection(out IShellItem item);void SetFileName([MarshalAs(UnmanagedType.LPWStr)]string name);void GetFileName(out IntPtr name);void SetTitle([MarshalAs(UnmanagedType.LPWStr)]string title);void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)]string label);void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)]string label);void GetResult(out IShellItem item);
        }
        [DllImport("shell32.dll",CharSet=CharSet.Unicode,PreserveSig=false)]static extern void SHCreateItemFromParsingName(string path,IntPtr bind,ref Guid iid,out IShellItem item);
        public static string Pick(Window owner,string title,string initial)
        {
            var dialog=(IFileDialog)new FileOpenDialog();IShellItem folder=null,result=null;
            try{
                dialog.SetOptions(0x20|0x40|0x800);dialog.SetTitle(title);
                if(!string.IsNullOrEmpty(initial)&&Directory.Exists(initial)){var iid=typeof(IShellItem).GUID;SHCreateItemFromParsingName(initial,IntPtr.Zero,ref iid,out folder);dialog.SetFolder(folder);}
                var hr=dialog.Show(new WindowInteropHelper(owner).Handle);if(hr==unchecked((int)0x800704c7))return null;if(hr!=0)Marshal.ThrowExceptionForHR(hr);
                dialog.GetResult(out result);IntPtr name;result.GetDisplayName(0x80058000,out name);
                try{return Marshal.PtrToStringUni(name);}finally{Marshal.FreeCoTaskMem(name);}
            }finally{if(result!=null)Marshal.ReleaseComObject(result);if(folder!=null)Marshal.ReleaseComObject(folder);Marshal.ReleaseComObject(dialog);}
        }
    }
}
