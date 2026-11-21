using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace OptScalerNR.Launcher
{
    public sealed partial class MainController
    {
        ProxyRecommendations _recommendations;
        readonly Dictionary<string,string> _proxyChoices=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string,bool> _optiPatcherByGame=new Dictionary<string,bool>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string,bool> _openRrByGame=new Dictionary<string,bool>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string,string> _openRrSpoofByGame=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        bool _selectingOptiPatcher;
        bool _selectingOpenRr;
        bool _selectingOpenRrSpoof;
        bool _selectingProxy;
        Button _refreshProfilesButton,_useRecommendationButton;
        TextBlock _profileStatus;
        void InitRecommendations()
        {
            _recommendations=new ProxyRecommendations(_settingsDir);
            _refreshProfilesButton=(Button)_window.FindName("RefreshProfilesButton");
            _useRecommendationButton=(Button)_window.FindName("UseRecommendationButton");
            _profileStatus=(TextBlock)_window.FindName("ProfileStatus");
            if(_refreshProfilesButton!=null)_refreshProfilesButton.Click+=(s,e)=>RefreshProfiles();
            if(_useRecommendationButton!=null)_useRecommendationButton.Click+=delegate{
                if(_selectedGame==null)return;_proxyChoices.Remove(_selectedGame.Id);SaveSettings();UpdateProxyRecommendation();
            };
        }
        HashSet<string> _optiPatcherSupportedExes;
        string _optiPatcherExesSource;
        bool _optiPatcherExesLoaded;
        HashSet<string> GetOptiPatcherSupportedExes()
        {
            string path=null;
            if(!string.IsNullOrEmpty(_packageRoot)){
                foreach(var rel in new[]{
                    Path.Combine("OptiScaler","plugins","OptiPatcher.asi"),
                    Path.Combine("cache","upstream","optipatcher","OptiScaler","plugins","OptiPatcher.asi")
                }){
                    var cand=Path.Combine(_packageRoot,rel);
                    if(File.Exists(cand)){path=cand;break;}
                }
            }
            if(path==null)return null;
            if(_optiPatcherExesLoaded&&string.Equals(_optiPatcherExesSource,path,StringComparison.OrdinalIgnoreCase))
                return _optiPatcherSupportedExes;
            try{
                var text=Encoding.ASCII.GetString(File.ReadAllBytes(path));
                var set=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach(Match m in Regex.Matches(text,@"[A-Za-z0-9_.\-+()]+\.exe",RegexOptions.CultureInvariant))
                    set.Add(m.Value);
                _optiPatcherSupportedExes=set;
                _optiPatcherExesSource=path;
                _optiPatcherExesLoaded=true;
                return set;
            }catch{
                _optiPatcherSupportedExes=null;
                _optiPatcherExesSource=path;
                _optiPatcherExesLoaded=true;
                return null;
            }
        }
        bool IsOptiPatcherSupported(GameInfo game,ProxyProfile profile)
        {
            if(game==null||!game.HasExecutable||game.IsRtxRemixMinecraft)return false;
            if(profile!=null&&profile.OptiPatcher)return true;
            var exes=GetOptiPatcherSupportedExes();
            if(exes==null||exes.Count==0)return true;
            return exes.Contains(Path.GetFileName(game.Exe));
        }
        void ApplyOptiPatcherTooltip(bool supported)
        {
            if(_optiPatcherToggle==null)return;
            _optiPatcherToggle.ToolTip=supported
                ?L("优先下载最新版本（30 秒时限），失败时回退包内离线副本，并启用 LoadAsiPlugins。","Downloads the latest version with a 30-second limit, falls back to the bundled offline copy, and enables LoadAsiPlugins.")
                :L("当前游戏不在内置 OptiPatcher 支持名单内，已自动禁用。","This game is not in the bundled OptiPatcher support list and has been disabled.");
        }
        void RememberOptiPatcherChoice()
        {
            if(!IsOptiScalerSelectedMode()||IsRtxRemixMinecraftSelected()){_decisions.OptiPatcher="skip";return;}
            bool active=_optiPatcherToggle!=null&&_optiPatcherToggle.IsEnabled&&_optiPatcherToggle.IsChecked==true;
            _decisions.OptiPatcher=active?"install":"skip";
            if(_selectingOptiPatcher||_loadingSettings||_applyingLanguage||_selectedGame==null||string.IsNullOrEmpty(_selectedGame.Id))return;
            if(_optiPatcherToggle!=null&&!_optiPatcherToggle.IsEnabled)return;
            _optiPatcherByGame[_selectedGame.Id]=active;
            SaveSettings();
        }
        void RememberOpenRrChoice()
        {
            if(!IsOptiScalerSelectedMode()||IsOpenRrUnsupportedLocked()){_decisions.OpenRr="skip";return;}
            _decisions.OpenRr=_openRrToggle!=null&&_openRrToggle.IsChecked==true?"install":"skip";
            if(_selectingOpenRr||_loadingSettings||_applyingLanguage||_selectedGame==null||string.IsNullOrEmpty(_selectedGame.Id))return;
            _openRrByGame[_selectedGame.Id]=_openRrToggle.IsChecked==true;
            SaveSettings();
        }
        void RememberOpenRrSpoofChoice()
        {
            bool optiActive=_optiPatcherToggle!=null&&_optiPatcherToggle.IsEnabled&&_optiPatcherToggle.IsChecked==true;
            if(_selectingOpenRrSpoof||_loadingSettings||_applyingLanguage||_selectedGame==null||string.IsNullOrEmpty(_selectedGame.Id))return;
            if(_openRrAutoSpoofToggle==null||!_openRrAutoSpoofToggle.IsEnabled||!optiActive){
                _decisions.OpenRrSpoof=(optiActive&&GetOpenRrSpoof(_selectedGame)=="auto")?"auto":"full";
                return;
            }
            _decisions.OpenRrSpoof=_openRrAutoSpoofToggle.IsChecked==true?"auto":"full";
            _openRrSpoofByGame[_selectedGame.Id]=_decisions.OpenRrSpoof;
            SaveSettings();
        }
        // Per-game GUI choice, default full. Always sent explicitly; the installer writes it as chosen.
        string GetOpenRrSpoof(GameInfo game)
        {
            string mode;
            return game!=null&&!string.IsNullOrEmpty(game.Id)&&_openRrSpoofByGame.TryGetValue(game.Id,out mode)?mode:"full";
        }
        void RememberProxyChoice()
        {
            if(_selectingProxy||_loadingSettings||_applyingLanguage||_selectedGame==null||string.IsNullOrEmpty(_selectedGame.Id))return;
            var proxy=CurrentSelectedProxy;
            if(ProxyRecommendations.Supports(proxy)){_proxyChoices[_selectedGame.Id]=proxy;SaveSettings();UpdateProxyRecommendation();}
        }
        void UpdateProxyRecommendation(string quickCandidate=null)
        {
            RefreshProxySummary();
            if(_recommendations==null)return;
            if(_refreshProfilesButton!=null)_refreshProfilesButton.Content=L("更新推荐资料","Refresh recommendations");
            if(_useRecommendationButton!=null){_useRecommendationButton.Content=L("使用推荐","Use suggestion");_useRecommendationButton.IsEnabled=_selectedGame!=null&&!_writeBusy;}
            if(_profileStatus!=null){
                var origin=_recommendations.UsingCache?L("缓存资料","Cached data"):L("随包资料","Bundled data");
                _profileStatus.Text=string.Format(L("{0}（{1} 款游戏）","{0} ({1} games)"),origin,_recommendations.Count);
                _profileStatus.ToolTip=string.Format(L("推荐规则来源：{0} · 共 {1} 款游戏 · 复核于 {2}","Recommendation source: {0} · {1} games · reviewed {2}"),origin,_recommendations.Count,_recommendations.Reviewed??"");
            }
            var profile=_selectedGame==null?null:_recommendations.Match(_selectedGame);
            if(!IsOptiScalerSelectedMode()){
                _selectingOptiPatcher=true;_selectingOpenRr=true;_selectingOpenRrSpoof=true;
                try{
                    if(_optiPatcherToggle!=null)_optiPatcherToggle.IsChecked=false;
                    if(_openRrToggle!=null)_openRrToggle.IsChecked=false;
                    if(_openRrAutoSpoofToggle!=null)_openRrAutoSpoofToggle.IsChecked=false;
                }finally{_selectingOptiPatcher=false;_selectingOpenRr=false;_selectingOpenRrSpoof=false;}
                _decisions.OptiPatcher="skip";_decisions.OpenRr="skip";_decisions.OpenRrSpoof="full";
                RefreshOpenRrCard();
                return;
            }
            bool optiSupported=IsOptiPatcherSupported(_selectedGame,profile);
            bool optiPatcher=false;
            if(optiSupported&&(_selectedGame==null||!_optiPatcherByGame.TryGetValue(_selectedGame.Id,out optiPatcher)))
                optiPatcher=profile!=null&&profile.OptiPatcher;
            if(!optiSupported)optiPatcher=false;
            _selectingOptiPatcher=true;
            try{
                if(_optiPatcherToggle!=null){
                    _optiPatcherToggle.IsEnabled=optiSupported&&!_writeBusy;
                    _optiPatcherToggle.IsChecked=optiPatcher;
                }
            }finally{_selectingOptiPatcher=false;}
            ApplyOptiPatcherTooltip(optiSupported);
            _decisions.OptiPatcher=optiPatcher?"install":"skip";
            bool openRrLocked=IsOpenRrUnsupportedLocked();
            bool openRr=false,savedOpenRr;
            if(!openRrLocked&&_selectedGame!=null&&_openRrByGame.TryGetValue(_selectedGame.Id,out savedOpenRr))openRr=savedOpenRr;
            _selectingOpenRr=true;
            try{
                if(_openRrToggle!=null){
                    _openRrToggle.IsEnabled=!openRrLocked;
                    _openRrToggle.IsChecked=openRr;
                }
            }
            finally{_selectingOpenRr=false;}
            _decisions.OpenRr=openRr?"install":"skip";
            bool openRrAutoSpoof=optiPatcher&&GetOpenRrSpoof(_selectedGame)=="auto";
            _selectingOpenRrSpoof=true;
            try{if(_openRrAutoSpoofToggle!=null)_openRrAutoSpoofToggle.IsChecked=openRrAutoSpoof;}
            finally{_selectingOpenRrSpoof=false;}
            _decisions.OpenRrSpoof=openRrAutoSpoof?"auto":"full";
            RefreshOpenRrCard();
            if(IsRtxRemixMinecraftSelected())return;
            if(_selectedGame==null){AppendMochizukiNote();return;}
            string manual;
            var hasManual=_proxyChoices.TryGetValue(_selectedGame.Id,out manual)&&ProxyRecommendations.Supports(manual);
            var guess=_lastInspectResult!=null?_lastInspectResult.Recommend:quickCandidate;
            var proxy=hasManual?manual:profile!=null?profile.Preferred:guess;
            SelectProxy(proxy);
            RefreshProxySummary();
            if(_injectRecommendation!=null){
                if(hasManual)_injectRecommendation.Text=L("使用你的手选代理：","Using your selected proxy: ")+manual;
                else if(profile!=null)_injectRecommendation.Text=L("Wiki 游戏规则建议：","Wiki game rule suggests: ")+profile.Preferred+
                    (profile.Alternatives.Length>0?L("；备选 ","; alternatives ")+string.Join(", ",profile.Alternatives):"");
                else if(!string.IsNullOrEmpty(guess))_injectRecommendation.Text=L("本地检测候选（未验证注入）：","Local candidate (injection not verified): ")+guess;
                else _injectRecommendation.Text=L("尚无已验证的游戏规则，也未找到可靠导入候选。请手动选择代理。","No reviewed game rule or reliable import candidate was found. Select a proxy manually.");
                if(profile!=null)_injectRecommendation.Text+="\n"+(_language=="zh"?profile.NotesZh:profile.NotesEn)+"\n"+profile.Source;
                AppendMochizukiNote();
            }
            if(_injectIcon!=null){_injectIcon.Text=hasManual||profile!=null?"✓":"ℹ";_injectIcon.Foreground=hasManual||profile!=null?ReadyBrush:WarningBrush;}
        }
        // Idempotent: every refresh of the recommendation text re-appends the coexistence note when
        // the last inspection found a Mochizuki installation, so no caller order can drop it.
        void AppendMochizukiNote()
        {
            if(_injectRecommendation==null)return;
            var mochi=_lastInspectResult==null?null:_lastInspectResult.MochizukiInstall;
            if(mochi==null||!mochi.Detected)return;
            if(_injectRecommendation.Text.Contains("DLSSNR-AMD"))return;
            _injectRecommendation.Text+="\n"+L("注意：检测到 Mochizuki（DLSSNR-AMD）自己的安装（"+string.Join(", ",mochi.Files??new string[0])+"）。同类 NR 模组共存可能冲突；安装器不会移动或删除这些文件。","Note: a Mochizuki (DLSSNR-AMD) installation is in this folder ("+string.Join(", ",mochi.Files??new string[0])+"). The same kind of NR mod twice can conflict; the installer never moves or deletes these files.");
            }
        async void RefreshProfiles()
        {
            if(_recommendations==null)return;
            if(_refreshProfilesButton!=null)_refreshProfilesButton.IsEnabled=false;
            if(_statusText!=null)SetStatus("正在从 OptiScaler 官方 Wiki 同步推荐资料…","Syncing recommendations from the official OptiScaler Wiki…");
            if(_statusLed!=null)_statusLed.Fill=WarningBrush;
            try{
                var outcome=await _recommendations.RefreshAsync();
                if(_window.Dispatcher.HasShutdownStarted)return;
                // Do not revise a choice while an installation or confirmation is in progress.
                if(!_writeBusy && _targetMutex==null && _window.OwnedWindows.Count==0)UpdateProxyRecommendation();
                AppendLog("recommendations: "+outcome);
                ShowRecommendationOutcome(outcome);
            }finally{if(_refreshProfilesButton!=null)_refreshProfilesButton.IsEnabled=true;}
        }
        void ShowRecommendationOutcome(string outcome)
        {
            var colon=outcome.IndexOf(':');
            int count;
            int.TryParse(colon<0?"":outcome.Substring(colon+1),out count);
            var kind=colon<0?outcome:outcome.Substring(0,colon);
            switch(kind){
                case "updated":
                    if(_statusText!=null)SetStatus(string.Format("已从 OptiScaler 官方 Wiki 同步 {0} 条游戏推荐规则。", count), string.Format("Synced {0} game recommendation rules from the official OptiScaler Wiki.", count));
                    if(_statusLed!=null)_statusLed.Fill=ReadyBrush;
                    break;
                case "not-modified":
                    if(_statusText!=null)SetStatus(string.Format("推荐资料已是最新（{0} 条规则）。", count), string.Format("Recommendations are already up to date ({0} rules).", count));
                    if(_statusLed!=null)_statusLed.Fill=ReadyBrush;
                    break;
                case "offline":
                    if(_statusText!=null)SetStatus("无法连接 OptiScaler Wiki，继续使用本地推荐资料。","Could not reach the OptiScaler Wiki; keeping local recommendations.");
                    if(_statusLed!=null)_statusLed.Fill=WarningBrush;
                    break;
                case "timeout":
                    if(_statusText!=null)SetStatus("连接 OptiScaler Wiki 超时，继续使用本地推荐资料。","Timed out contacting the OptiScaler Wiki; keeping local recommendations.");
                    if(_statusLed!=null)_statusLed.Fill=WarningBrush;
                    break;
                case "invalid":
                    if(_statusText!=null)SetStatus("OptiScaler Wiki 返回无法解析的内容，继续使用本地推荐资料。","The OptiScaler Wiki returned unusable content; keeping local recommendations.");
                    if(_statusLed!=null)_statusLed.Fill=WarningBrush;
                    break;
                case "busy":
                    if(_statusText!=null)SetStatus("正在同步推荐资料，请稍候。","Recommendation sync is already running.");
                    break;
            }
        }
    }
}
