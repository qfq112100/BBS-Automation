#requires -Version 5.1
<#
Run with Windows PowerShell: powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\ui_regression.ps1 -Executable C:\absolute\BBS.exe -OutputDir $env:TEMP\bbs-ui-unique
Only the newly launched PID is addressed. No global mouse/keyboard injection is used.
Physical desktop pixels are read only inside that PID's visible client/popup rectangles.
-Keep retains the isolated runtime root; reports/screenshots are always retained.
-KillOwnedOnTimeout explicitly permits force cleanup of ONLY the launched PID after safe shutdown times out.
Sampled frame stability is not proof of absence of flicker between samples.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [ValidateSet(96,120,144)][int]$Dpi = 96,
    [switch]$Keep,
    [switch]$KillOwnedOnTimeout,
    [switch]$Light,
    [switch]$TestLogWheel
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$checks = New-Object 'System.Collections.Generic.List[object]'
$regionSamples = New-Object 'System.Collections.Generic.List[object]'
$idleMenu=$null
$proc = $null; $main = [IntPtr]::Zero; $fatal = $null; $cleanup = 'not-started'
$started = [DateTime]::UtcNow
$coverage='not-run'; $coverageReason=$null; $chosenSize=$null
if (-not [IO.Path]::IsPathRooted($Executable) -or -not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'Executable must be an existing absolute executable path.' }
$Executable = [IO.Path]::GetFullPath($Executable)
$tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (-not $OutputDir.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $OutputDir)) { throw 'OutputDir must be a fresh nonexistent directory beneath the current user TEMP directory.' }
[void](New-Item -ItemType Directory -Path $OutputDir)
$runtimeRoot = Join-Path $OutputDir 'isolated-runtime'
$nativeSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
public static class BbsUiProbe {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; public RECT(int l,int t,int r,int b){Left=l;Top=t;Right=r;Bottom=b;} public override string ToString(){return Left+","+Top+","+Right+","+Bottom;} }
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; public POINT(int x,int y){X=x;Y=y;} }
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h,EnumProc cb,IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 public static string Caption(IntPtr h){var s=new StringBuilder(256);GetWindowText(h,s,s.Capacity);return s.ToString();}
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
 [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from,IntPtr to,ref RECT r,uint n);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
 [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int how);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr z,int x,int y,int w,int he,uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,IntPtr l,uint f,uint ms,out IntPtr result);
 [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendStringTimeout(IntPtr h,uint m,IntPtr w,string text,uint f,uint ms,out IntPtr result);
 public static void SetOwnedText(IntPtr h,uint pid,string text){uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid)throw new Exception("Refusing foreign text fixture");IntPtr result;if(SendStringTimeout(h,12,IntPtr.Zero,text,2,1500,out result)==IntPtr.Zero||result==IntPtr.Zero)throw new Exception("Owned log text fixture failed");}
 [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h,IntPtr rect,IntPtr rgn,uint flags);
 [DllImport("user32.dll")] public static extern int GetWindowRgn(IntPtr h,IntPtr r);
 [DllImport("gdi32.dll")] public static extern IntPtr CreateRectRgn(int l,int t,int r,int b);
 [DllImport("gdi32.dll")] public static extern bool EqualRgn(IntPtr a,IntPtr b);
 [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
 [DllImport("gdi32.dll")] public static extern int GetRgnBox(IntPtr r,out RECT box);
 [DllImport("kernel32.dll")] public static extern int MulDiv(int n,int numer,int denom);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("dwmapi.dll")] public static extern int DwmFlush();
 public static void PhysicalCoordinates(){try{SetThreadDpiAwarenessContext(new IntPtr(-4));}catch(EntryPointNotFoundException){SetProcessDPIAware();}}
 public static IntPtr Send(IntPtr h,uint m,long w,long l){IntPtr result; if(SendMessageTimeout(h,m,new IntPtr(w),new IntPtr(l),2,1500,out result)==IntPtr.Zero)throw new Exception("Owned window message timeout: "+m);return result;}
 public static long XY(int x,int y){return (long)((uint)(ushort)x | ((uint)(ushort)y<<16));}
 public static IntPtr Find(uint pid,string cls){IntPtr found=IntPtr.Zero; EnumWindows(delegate(IntPtr h,IntPtr p){uint id;GetWindowThreadProcessId(h,out id);var s=new StringBuilder(256);GetClassName(h,s,s.Capacity);if(id==pid&&s.ToString()==cls){found=h;return false;}return true;},IntPtr.Zero);return found;}
 public static IntPtr[] Children(IntPtr parent,uint pid){var list=new List<IntPtr>();EnumChildWindows(parent,delegate(IntPtr h,IntPtr p){uint id;GetWindowThreadProcessId(h,out id);if(id==pid&&GetParent(h)==parent)list.Add(h);return true;},IntPtr.Zero);return list.ToArray();}
 public static RECT Client(IntPtr h){RECT r;if(!GetClientRect(h,out r))throw new Exception("GetClientRect failed");return r;}
 public static RECT InParent(IntPtr h,IntPtr parent){RECT r;if(!GetWindowRect(h,out r))throw new Exception("GetWindowRect failed");MapWindowPoints(IntPtr.Zero,parent,ref r,2);return r;}
 public static int D(int n,int dpi){return MulDiv(n,dpi,96);}
 public static RECT Body(IntPtr h,int dpi){RECT r=Client(h);int w=MulDiv(r.Right,96,dpi),he=MulDiv(r.Bottom,96,dpi);return new RECT(D(224,dpi),D(160,dpi),D(w,dpi),D(Math.Max(160,he-48),dpi));}
 public static RECT Intersect(RECT a,RECT b){int l=Math.Max(a.Left,b.Left),t=Math.Max(a.Top,b.Top),r=Math.Min(a.Right,b.Right),bt=Math.Min(a.Bottom,b.Bottom);return r<=l||bt<=t?new RECT(0,0,0,0):new RECT(l,t,r,bt);}
 public static bool RegionMatches(IntPtr h,RECT child,RECT body,out int kind,out string actual,out string expected){RECT v=Intersect(child,body);RECT e=v.Right<=v.Left||v.Bottom<=v.Top?new RECT(0,0,0,0):new RECT(v.Left-child.Left,v.Top-child.Top,v.Right-child.Left,v.Bottom-child.Top);IntPtr a=CreateRectRgn(0,0,0,0),b=CreateRectRgn(e.Left,e.Top,e.Right,e.Bottom);try{kind=GetWindowRgn(h,a);RECT box;GetRgnBox(a,out box);actual=box.ToString();expected=e.ToString();return kind!=0&&EqualRgn(a,b);}finally{DeleteObject(a);DeleteObject(b);}}
 public static Bitmap Capture(IntPtr h,uint pid,string path){DwmFlush();RECT r=Client(h);var p=new POINT(0,0);ClientToScreen(h,ref p);var bounds=new Rectangle(p.X,p.Y,r.Right,r.Bottom);if(!SystemInformation.VirtualScreen.Contains(bounds))throw new Exception("Own client exceeds physical virtual desktop: "+bounds);foreach(var q in new POINT[]{new POINT(p.X+4,p.Y+4),new POINT(p.X+r.Right-5,p.Y+4),new POINT(p.X+4,p.Y+r.Bottom-5),new POINT(p.X+r.Right/2,p.Y+r.Bottom/2)}){uint owner;GetWindowThreadProcessId(WindowFromPoint(q),out owner);if(owner!=pid)throw new Exception("Own capture is occluded by another PID; refusing unrelated screen capture");}var b=new Bitmap(r.Right,r.Bottom,PixelFormat.Format32bppArgb);using(var g=Graphics.FromImage(b)){g.CopyFromScreen(p.X,p.Y,0,0,b.Size,CopyPixelOperation.SourceCopy);}b.Save(path,ImageFormat.Png);return b;}
 static int[] Pixels(Bitmap b){var r=new Rectangle(0,0,b.Width,b.Height);var d=b.LockBits(r,ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);try{int[] p=new int[b.Width*b.Height];for(int y=0;y<b.Height;y++)Marshal.Copy(IntPtr.Add(d.Scan0,y*d.Stride),p,y*b.Width,b.Width);return p;}finally{b.UnlockBits(d);}}
 static bool Inside(int x,int y,RECT r){return x>=r.Left&&x<r.Right&&y>=r.Top&&y<r.Bottom;}
 public static long Diff(Bitmap a,Bitmap b,RECT include,RECT[] excludes){if(a.Size!=b.Size)throw new Exception("Capture dimensions changed");var p=Pixels(a);var q=Pixels(b);long count=0;for(int y=Math.Max(0,include.Top);y<Math.Min(a.Height,include.Bottom);y++)for(int x=Math.Max(0,include.Left);x<Math.Min(a.Width,include.Right);x++){bool skip=false;foreach(var ex in excludes)if(Inside(x,y,ex)){skip=true;break;}if(!skip&&p[y*a.Width+x]!=q[y*a.Width+x])count++;}return count;}
 public static long TextEdges(Bitmap b,RECT row,int dpi){var p=Pixels(b);long edges=0;for(int y=Math.Max(0,row.Top+3);y<Math.Min(b.Height,row.Bottom-3);y++)for(int x=Math.Max(1,D(32,dpi));x<Math.Min(b.Width-D(12,dpi),row.Right);x++){int a=p[y*b.Width+x],c=p[y*b.Width+x-1];int delta=Math.Abs((a&255)-(c&255))+Math.Abs(((a>>8)&255)-((c>>8)&255))+Math.Abs(((a>>16)&255)-((c>>16)&255));if(delta>45)edges++;}return edges;}
 public static RECT WorkingArea(){var r=Screen.PrimaryScreen.WorkingArea;return new RECT(r.Left,r.Top,r.Right,r.Bottom);}
}
'@
function Assert-Probe([bool]$Pass,[string]$Name,$Details) { $checks.Add([pscustomobject]@{pass=$Pass;name=$Name;details=$Details}) }
function Send-Owned([IntPtr]$Window,[uint32]$Message,[long]$W=0,[long]$L=0) {
    [uint32]$ownerId=0; [void][BbsUiProbe]::GetWindowThreadProcessId($Window,[ref]$ownerId)
    if ($ownerId -ne $proc.Id) { throw 'Refusing to send a message to any non-owned window.' }
    return [BbsUiProbe]::Send($Window,$Message,$W,$L)
}
function Wait-OwnedWindow([string]$Class,[int]$Milliseconds=3000) {
    $until=[DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    do { $h=[BbsUiProbe]::Find([uint32]$proc.Id,$Class); if ($h -ne [IntPtr]::Zero -and [BbsUiProbe]::IsWindowVisible($h)) { return $h }; Start-Sleep -Milliseconds 30 } while ([DateTime]::UtcNow -lt $until -and -not $proc.HasExited)
    throw "Timed out finding own window class $Class"
}
function Check-Regions([string]$Stage) {
    $body=[BbsUiProbe]::Body($main,$Dpi); $tested=0
    foreach ($h in [BbsUiProbe]::Children($main,[uint32]$proc.Id)) {
        $id=[BbsUiProbe]::GetDlgCtrlID($h)
        if ($id -lt 200 -or $id -eq 250 -or -not [BbsUiProbe]::IsWindowVisible($h)) { continue }
        $r=[BbsUiProbe]::InParent($h,$main); [int]$kind=0; [string]$actual=''; [string]$expected=''
        $ok=[BbsUiProbe]::RegionMatches($h,$r,$body,[ref]$kind,[ref]$actual,[ref]$expected)
        $sample=[pscustomobject]@{stage=$Stage;id=$id;rect=$r.ToString();body=$body.ToString();kind=$kind;actual=$actual;expected=$expected;pass=$ok}
        $regionSamples.Add($sample); Assert-Probe $ok "region:$Stage/id$id" $sample; $tested++
    }
    Assert-Probe ($tested -gt 0) "region-controls:$Stage" $tested
}
function Check-LiveRepair([string]$Stage) {
    [void](Send-Owned $main 0); Start-Sleep -Milliseconds 25
    $before=$null; $after=$null
    try {
        $before=[BbsUiProbe]::Capture($main,[uint32]$proc.Id,(Join-Path $OutputDir "$Stage-before-repair.png"))
        [void][BbsUiProbe]::RedrawWindow($main,[IntPtr]::Zero,[IntPtr]::Zero,0x181)
        [void](Send-Owned $main 0); [void][BbsUiProbe]::DwmFlush()
        $after=[BbsUiProbe]::Capture($main,[uint32]$proc.Id,(Join-Path $OutputDir "$Stage-after-repair.png"))
        $body=[BbsUiProbe]::Body($main,$Dpi); $r=[BbsUiProbe]::Client($main)
        $header=[BbsUiProbe+RECT]::new($body.Left,[BbsUiProbe]::D(56,$Dpi),$r.Right,$body.Top)
        $footer=[BbsUiProbe+RECT]::new($body.Left,$body.Bottom,$r.Right,$r.Bottom)
        # Compare the entire footer, including notices; forced launch commands run after all captures.
        $exclude=@()
        $headDiff=[BbsUiProbe]::Diff($before,$after,$header,[BbsUiProbe+RECT[]]@())
        $footDiff=[BbsUiProbe]::Diff($before,$after,$footer,[BbsUiProbe+RECT[]]$exclude)
        Assert-Probe ($headDiff -eq 0) "live-header-stale:$Stage" @{changedPixels=$headDiff}
        Assert-Probe ($footDiff -eq 0) "live-footer-stale:$Stage" @{changedPixels=$footDiff;noticeMask=$false}
    } finally { if($before){$before.Dispose()}; if($after){$after.Dispose()} }
}
function Check-OverviewQuickOpen([string]$Stage,[bool]$Visible) {
    $client=[BbsUiProbe]::Client($main)
    $header=[BbsUiProbe+RECT]::new([BbsUiProbe]::D(224,$Dpi),[BbsUiProbe]::D(56,$Dpi),$client.Right,[BbsUiProbe]::D(160,$Dpi))
    $rects=@()
    foreach($id in @(112,113,114)) {
        $matches=@([BbsUiProbe]::Children($main,[uint32]$proc.Id) | Where-Object {[BbsUiProbe]::GetDlgCtrlID($_) -eq $id})
        Assert-Probe ($matches.Count -eq 1) "overview-native-unique:$Stage/id$id" @{count=$matches.Count}
        if($matches.Count -ne 1){continue}
        $h=$matches[0]; [uint32]$ownerId=0
        [void][BbsUiProbe]::GetWindowThreadProcessId($h,[ref]$ownerId)
        $class=New-Object Text.StringBuilder 256; [void][BbsUiProbe]::GetClassName($h,$class,$class.Capacity)
        $caption=switch($id){112{'打开 Rose'}113{'打开 Akari'}114{'打开 ABC'}}
        Assert-Probe ($ownerId -eq $proc.Id -and [BbsUiProbe]::GetParent($h) -eq $main -and $class.ToString() -eq 'Button' -and [BbsUiProbe]::Caption($h) -eq $caption -and $h -eq $quickHandles[$id]) "overview-native-identity:$Stage/id$id" @{hwnd=$h.ToInt64();owner=$ownerId;class=$class.ToString();caption=[BbsUiProbe]::Caption($h)}
        Assert-Probe ([BbsUiProbe]::IsWindowVisible($h) -eq $Visible) "overview-only-visibility:$Stage/id$id" @{expected=$Visible;actual=[BbsUiProbe]::IsWindowVisible($h)}
        Assert-Probe (-not [BbsUiProbe]::IsWindowEnabled($h)) "overview-smoke-disabled:$Stage/id$id" $null
        if(-not $Visible){continue}
        $r=[BbsUiProbe]::InParent($h,$main); $rects+=$r
        Assert-Probe ($r.Right -gt $r.Left -and $r.Bottom -gt $r.Top -and $r.Left -ge $header.Left -and $r.Top -ge $header.Top -and $r.Right -lt $header.Right -and $r.Bottom -le $header.Bottom) "overview-fixed-header-contained:$Stage/id$id" @{rect=$r.ToString();header=$header.ToString();rightMargin=$client.Right-$r.Right;dpi=$Dpi}
        # Frozen right-anchored cluster: ABC left, Rose middle, Akari right; all share y/size.
        $rightOffset=switch($id){112{264}113{144}114{384}}
        $expected=[BbsUiProbe+RECT]::new($client.Right-[BbsUiProbe]::D($rightOffset,$Dpi),[BbsUiProbe]::D(88,$Dpi),$client.Right-[BbsUiProbe]::D(($rightOffset-112),$Dpi),[BbsUiProbe]::D(120,$Dpi))
        Assert-Probe ($r.ToString() -eq $expected.ToString()) "overview-fixed-header-bounds:$Stage/id$id" @{rect=$r.ToString();expected=$expected.ToString();dpi=$Dpi}
        Assert-Probe ($r.Left -ge [BbsUiProbe]::D(556,$Dpi)) "overview-clear-left-heading:$Stage/id$id" @{rect=$r.ToString();minimumLogicalLeft=556;header=$header.ToString()}
    }
    if($Visible -and $rects.Count -eq 3){
        $rose=$rects[0]; $akari=$rects[1]; $abc=$rects[2]
        $roseAkari=[BbsUiProbe]::Intersect($rose,$akari); $abcRose=[BbsUiProbe]::Intersect($abc,$rose); $abcAkari=[BbsUiProbe]::Intersect($abc,$akari)
        Assert-Probe ($roseAkari.Right -le $roseAkari.Left -and $abcRose.Right -le $abcRose.Left -and $abcAkari.Right -le $abcAkari.Left -and $abc.Right -lt $rose.Left -and $rose.Right -lt $akari.Left) "overview-quick-open-nonoverlap:$Stage" @{abc=$abc.ToString();rose=$rose.ToString();akari=$akari.ToString()}
        Assert-Probe ($rose.Left-$abc.Right -eq [BbsUiProbe]::D(8,$Dpi) -and $akari.Left-$rose.Right -eq [BbsUiProbe]::D(8,$Dpi) -and $client.Right-$akari.Right -eq [BbsUiProbe]::D(32,$Dpi)) "overview-quick-open-gaps-right-margin:$Stage" @{abcRoseGap=$rose.Left-$abc.Right;roseAkariGap=$akari.Left-$rose.Right;rightMargin=$client.Right-$akari.Right;dpi=$Dpi}
    }
}
function Check-NoToolExtraction([string]$Stage) {
    # App::Initialize passes smoke root/settings to ToolManager, not the top-level smoke root.
    $settings=Join-Path $runtimeRoot 'settings'
    $components=@((Join-Path $settings 'components'),(Join-Path $runtimeRoot 'components'))
    $found=@($components | Where-Object {Test-Path -LiteralPath $_})
    $payloads=@(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -File -ErrorAction Stop | Where-Object {$_.Extension -in @('.exe','.dll','.zip','.7z')})
    Assert-Probe (Test-Path -LiteralPath $settings -PathType Container) "overview-isolated-settings-root:$Stage" $settings
    Assert-Probe ($found.Count -eq 0 -and $payloads.Count -eq 0) "overview-no-tool-extraction:$Stage" @{checkedComponents=$components;foundComponents=$found;payloads=@($payloads | ForEach-Object {$_.FullName})}
}
function Open-OwnedTray {
    if (-not [BbsUiProbe]::PostMessage($main,0x8002,[IntPtr]::Zero,[IntPtr]0x7B)) { throw 'Tray open PostMessage failed' }
    $menu=Wait-OwnedWindow 'BBS.TrayMenu'
    # Visibility/hit testing can precede the first DWM-presented popup frame.
    # Synchronize initial paint before measuring hover; do not compare the underlying desktop.
    [void][BbsUiProbe]::RedrawWindow($menu,[IntPtr]::Zero,[IntPtr]::Zero,0x185)
    [void](Send-Owned $menu 0 0 0)
    [void][BbsUiProbe]::DwmFlush(); [void][BbsUiProbe]::DwmFlush()
    if((Send-Owned $menu 0x8155 0 0).ToInt32() -ne 1){throw "Owned smoke popup did not enable isolated synthetic input"}
    return $menu
}
try {
    Add-Type -TypeDefinition $nativeSource -ReferencedAssemblies System.Drawing,System.Windows.Forms
    [BbsUiProbe]::PhysicalCoordinates()
    $work=[BbsUiProbe]::WorkingArea()
    $fitWidth=[int][Math]::Floor(($work.Right-$work.Left-16)*96.0/$Dpi)
    $fitHeight=[int][Math]::Floor(($work.Bottom-$work.Top-16)*96.0/$Dpi)
    if($fitWidth -lt 940 -or $fitHeight -lt 640){$coverage='not-covered';$coverageReason="DPI $Dpi minimum 940x640 cannot fit physical working area with 8px margins.";throw $coverageReason}
    $chosenSize=@([Math]::Min(983,$fitWidth),[Math]::Min(727,$fitHeight))
    $coverage='running'
    $argsText='--smoke "' + $runtimeRoot + '" --smoke-page 4 --smoke-size ' + $chosenSize[0] + ' ' + $chosenSize[1] + ' --smoke-duration 120000 --smoke-dpi ' + $Dpi
    if($Light){$argsText += " --smoke-light"}
    $proc=Start-Process -FilePath $Executable -ArgumentList $argsText -PassThru
    $main=Wait-OwnedWindow 'LOLBackupV3.NativeWindow' 10000
    [void][BbsUiProbe]::ShowWindow($main,9); [void][BbsUiProbe]::SetForegroundWindow($main)
    [void][BbsUiProbe]::SetWindowPos($main,[IntPtr]::Zero,$work.Left+8,$work.Top+8,[BbsUiProbe]::D($chosenSize[0],$Dpi),[BbsUiProbe]::D($chosenSize[1],$Dpi),0)
    Start-Sleep -Milliseconds 350
    $ids=@([BbsUiProbe]::Children($main,[uint32]$proc.Id) | ForEach-Object { [BbsUiProbe]::GetDlgCtrlID($_) })
    $expectedToolIds=@(600,603,610,613,620,623)
    foreach($id in $expectedToolIds){ Assert-Probe (@($ids | Where-Object {$_ -eq $id}).Count -eq 1) "tool-created:$id" $null }
    Assert-Probe (@($ids | Where-Object {$_ -ge 600 -and $_ -le 629}).Count -eq 6) "tool-control-count-six" @{expected=$expectedToolIds;actual=@($ids | Where-Object {$_ -ge 600 -and $_ -le 629})}
    foreach($id in 600..629){if($id -notin $expectedToolIds){Assert-Probe (-not ($ids -contains $id)) "no-legacy-or-ltk-or-dependency-control:$id" $null}}
    Assert-Probe (((Send-Owned $main 0x8041 0 0).ToInt32()) -eq 1) "first-card-full-official-rose" $null
    Assert-Probe (((Send-Owned $main 0x8041 1 0).ToInt32()) -eq 2) "second-card-league-akari" $null
    Assert-Probe (((Send-Owned $main 0x8041 2 0).ToInt32()) -eq 4) "third-card-abc" $null
    Assert-Probe (((Send-Owned $main 0x8041 3 0).ToInt32()) -eq 0) "no-ltk-or-dependency-card" $null
    foreach($h in [BbsUiProbe]::Children($main,[uint32]$proc.Id)){
        $id=[BbsUiProbe]::GetDlgCtrlID($h)
        if($id -in @(600,610,620)){
            Assert-Probe (-not [BbsUiProbe]::IsWindowEnabled($h)) "smoke-open-disabled:$id" $null
            Assert-Probe ([BbsUiProbe]::Caption($h) -eq "打开") "simple-open-caption:$id" $null
        }
        if($id -in @(603,613,623)){Assert-Probe ([BbsUiProbe]::Caption($h) -eq "打开目录") "directory-caption:$id" $null}
    }
    Check-Regions 'initial'; Check-LiveRepair 'initial'
    $deltas=@(-120,-120,-120,120,-120,120,-120,-120,-120,-120,-120,-120,120,120,120,120,120,120,120,120)
    for($i=0;$i -lt $deltas.Count;$i++) {
        [void](Send-Owned $main 0x20A ([long]$deltas[$i]*65536) 0)
        Check-Regions ("wheel-{0:d2}" -f $i)
        if($i -in @(2,7,11,19)){Check-LiveRepair ("wheel-{0:d2}" -f $i)}
    }
    for($page=0;$page -le 4;$page++) {
        [void](Send-Owned $main 0x111 (100+$page) 0); Check-Regions "page$page"
        [void](Send-Owned $main 0x20A (-120*65536) 0); Check-Regions "page$page-wheel"
        Check-LiveRepair "page$page"
    }
    foreach($size in @(@(940,640),$chosenSize)) {
        [void][BbsUiProbe]::SetWindowPos($main,[IntPtr]::Zero,$work.Left+8,$work.Top+8,[BbsUiProbe]::D($size[0],$Dpi),[BbsUiProbe]::D($size[1],$Dpi),0)
        Check-Regions ("resize-{0}x{1}" -f $size[0],$size[1]); Check-LiveRepair ("resize-{0}x{1}" -f $size[0],$size[1])
    }
    foreach($h in [BbsUiProbe]::Children($main,[uint32]$proc.Id)) {
        $id=[BbsUiProbe]::GetDlgCtrlID($h)
        if($id -ge 600 -and [BbsUiProbe]::IsWindowVisible($h)) { [void](Send-Owned $h 0x200 0 ([BbsUiProbe]::XY(10,10))); [void](Send-Owned $h 0x2A3); Check-Regions "hover$id" }
    }
    Check-LiveRepair 'tool-hover'
    $popup=Open-OwnedTray
    $rows=New-Object 'System.Collections.Generic.List[object]'; $top=[BbsUiProbe]::D(4,$Dpi)
    for($i=0;$i -lt 11;$i++){
        $separator=$i -in @(1,5,9); $height=[BbsUiProbe]::D($(if($separator){7}else{23}),$Dpi)
        $client=[BbsUiProbe]::Client($popup)
        $rows.Add([pscustomobject]@{index=$i;separator=$separator;rect=[BbsUiProbe+RECT]::new(0,$top,$client.Right,$top+$height)})
        $top+=$height
    }
    $previous=[BbsUiProbe+RECT]::new(0,0,0,0)
    foreach($row in $rows){
        $a=$null;$b=$null;$c=$null
        try {
            $a=[BbsUiProbe]::Capture($popup,[uint32]$proc.Id,(Join-Path $OutputDir "tray-row$($row.index)-before.png"))
            [void](Send-Owned $popup 0x200 0 ([BbsUiProbe]::XY([BbsUiProbe]::D(40,$Dpi),[int](($row.rect.Top+$row.rect.Bottom)/2))))
            # Open-OwnedTray isolates synthetic input on the owned smoke popup UI thread.
            # The normal leave handler is verified explicitly after this hover matrix.
            Start-Sleep -Milliseconds 20
            $b=[BbsUiProbe]::Capture($popup,[uint32]$proc.Id,(Join-Path $OutputDir "tray-row$($row.index)-frame1.png"))
            Start-Sleep -Milliseconds 35
            $c=[BbsUiProbe]::Capture($popup,[uint32]$proc.Id,(Join-Path $OutputDir "tray-row$($row.index)-frame2.png"))
            $all=[BbsUiProbe]::Client($popup)
            $outside=[BbsUiProbe]::Diff($a,$b,$all,[BbsUiProbe+RECT[]]@($previous,$row.rect))
            $stable=[BbsUiProbe]::Diff($b,$c,$all,[BbsUiProbe+RECT[]]@())
            Assert-Probe ($outside -eq 0) "tray-hover-outside-row$($row.index)" @{changedPixels=$outside}
            Assert-Probe ($stable -eq 0) "tray-two-samples-row$($row.index)" @{changedPixels=$stable;sampleGapMs=35}
            foreach($textRow in $rows){if(-not $textRow.separator){$edges=[BbsUiProbe]::TextEdges($c,$textRow.rect,$Dpi);Assert-Probe ($edges -gt 6) "tray-text-row$($row.index)-text$($textRow.index)" @{edgeTransitions=$edges;method='nonblank raster heuristic, not OCR'}}}
            if($row.separator){if($idleMenu){$idleMenu.Dispose()};$idleMenu=$c.Clone()}
            $previous=$row.rect
        } finally {if($a){$a.Dispose()};if($b){$b.Dispose()};if($c){$c.Dispose()}}
    }
    [void](Send-Owned $popup 0x2A3 0 0);Start-Sleep -Milliseconds 20
    $left=[BbsUiProbe]::Capture($popup,[uint32]$proc.Id,(Join-Path $OutputDir "tray-explicit-leave.png"))
    try{$diff=[BbsUiProbe]::Diff($idleMenu,$left,[BbsUiProbe]::Client($popup),[BbsUiProbe+RECT[]]@());Assert-Probe ($diff -eq 0) "tray-explicit-mouse-leave-clears-hover" @{changedPixels=$diff}}finally{$left.Dispose();$idleMenu.Dispose();$idleMenu=$null}
    foreach($key in @(0x28,0x28,0x26)){[void](Send-Owned $popup 0x100 $key 0)}
    Assert-Probe ([BbsUiProbe]::IsWindow($popup)) 'tray-keyboard-navigation-keeps-menu' $null
    [void](Send-Owned $popup 0x100 0x1B 0); Start-Sleep -Milliseconds 30
    Assert-Probe (-not [BbsUiProbe]::IsWindow($popup)) 'tray-escape-dismisses' $null
    $popup=Open-OwnedTray
    [void](Send-Owned $popup 0x201 0 ([BbsUiProbe]::XY(-10,-10))); Start-Sleep -Milliseconds 30
    Assert-Probe (-not [BbsUiProbe]::IsWindow($popup)) 'tray-outside-direct-message-dismisses' $null
    if($TestLogWheel){
        [void](Send-Owned $main 0x111 102 0); Start-Sleep -Milliseconds 150
        $log=@([BbsUiProbe]::Children($main,[uint32]$proc.Id) | Where-Object {[BbsUiProbe]::GetDlgCtrlID($_) -eq 303})[0]
        $fixture=((0..99 | ForEach-Object {'UI wheel fixture line {0:d3}' -f $_}) -join "`r`n")
        foreach($route in @('edit','parent')){
            [BbsUiProbe]::SetOwnedText($log,[uint32]$proc.Id,$fixture)
            $target=$log;if($route -eq 'parent'){$target=$main}
            [void](Send-Owned $log 0xB6 0 -10000)
            [void](Send-Owned $target 0x20A (-120*65536) 0);$whole=(Send-Owned $log 0xCE).ToInt32()
            [void](Send-Owned $log 0xB6 0 -10000)
            for($part=0;$part -lt 4;$part++){[void](Send-Owned $target 0x20A (-30*65536) 0)}
            $fractional=(Send-Owned $log 0xCE).ToInt32()
            Assert-Probe ($whole -gt 0 -and $whole -eq $fractional) "log-fractional-wheel:$route" @{one120=$whole;four30=$fractional;fixtureLines=100}
        }
    }
    # Append Overview coverage only after the unchanged Tool Center/tray/log matrix.
    # Forced launch commands remain below EVERY capture/header/footer comparison: their notices expire.
    $quickHandles=@{}
    foreach($h in [BbsUiProbe]::Children($main,[uint32]$proc.Id)){
        $id=[BbsUiProbe]::GetDlgCtrlID($h); if($id -in @(112,113,114)){$quickHandles[$id]=$h}
    }
    foreach($page in @(1,2,3,4,0)){
        [void](Send-Owned $main 0x111 (100+$page) 0)
        Check-OverviewQuickOpen "page$page" ($page -eq 0)
    }
    $pinned=@{}
    foreach($id in @(112,113,114)){if($quickHandles.ContainsKey($id)){$pinned[$id]=[BbsUiProbe]::InParent($quickHandles[$id],$main)}}
    Check-LiveRepair 'overview-quick-open-initial'
    foreach($delta in @(-120,-120,-120,120,120,120)){
        [void](Send-Owned $main 0x20A ([long]$delta*65536) 0)
        foreach($id in @(112,113,114)){
            if(-not $pinned.ContainsKey($id)){continue}
            $r=[BbsUiProbe]::InParent($quickHandles[$id],$main)
            Assert-Probe ($r.ToString() -eq $pinned[$id].ToString()) "overview-wheel-pinned:$delta/id$id" @{before=$pinned[$id].ToString();after=$r.ToString();dpi=$Dpi}
        }
    }
    Check-OverviewQuickOpen 'after-wheel' $true
    Check-LiveRepair 'overview-quick-open-wheel'
    foreach($size in @(@(940,640),$chosenSize)){
        [void][BbsUiProbe]::SetWindowPos($main,[IntPtr]::Zero,$work.Left+8,$work.Top+8,[BbsUiProbe]::D($size[0],$Dpi),[BbsUiProbe]::D($size[1],$Dpi),0)
        Check-OverviewQuickOpen ("resize-{0}x{1}" -f $size[0],$size[1]) $true
        foreach($id in @(112,113,114)){
            if(-not $pinned.ContainsKey($id)){continue}
            $r=[BbsUiProbe]::InParent($quickHandles[$id],$main)
            Assert-Probe ($r.Top -eq $pinned[$id].Top -and $r.Bottom -eq $pinned[$id].Bottom) "overview-resize-pinned-y:$($size[0])/id$id" @{before=$pinned[$id].ToString();after=$r.ToString();dpi=$Dpi}
        }
        Check-LiveRepair ("overview-resize-{0}x{1}" -f $size[0],$size[1])
    }
    $overviewShot=[BbsUiProbe]::Capture($main,[uint32]$proc.Id,(Join-Path $OutputDir 'overview-quick-open.png'))
    $overviewShot.Dispose()
    # Disabled owner-draw controls have no hover state; do not fabricate an enabled hover screenshot.
    Check-NoToolExtraction 'before-forced-command'
    foreach($id in @(112,113,114)){
        [void](Send-Owned $main 0x111 $id 0)
        [void](Send-Owned $main 0)
        Start-Sleep -Milliseconds 150
        Assert-Probe (-not $proc.HasExited -and [BbsUiProbe]::IsWindow($main)) "overview-native-command-guard-alive:$id" @{pid=$proc.Id;command=$id;scope='owned PID only; no real-user application query or termination'}
        if($quickHandles.ContainsKey($id)){Assert-Probe (-not [BbsUiProbe]::IsWindowEnabled($quickHandles[$id])) "overview-native-command-remains-disabled:$id" $null}
        Check-NoToolExtraction "after-forced-command$id"
    }
} catch {
    $fatal=$_.Exception.ToString(); Assert-Probe $false 'harness-exception' $fatal
} finally {
    if($idleMenu){$idleMenu.Dispose()}
    if($proc){
        try {
            if(-not $proc.HasExited){
                $ownedPopup=[BbsUiProbe]::Find([uint32]$proc.Id,'BBS.TrayMenu')
                if($ownedPopup -ne [IntPtr]::Zero){[void](Send-Owned $ownedPopup 0x100 0x1B 0)}
                if($main -ne [IntPtr]::Zero -and [BbsUiProbe]::IsWindow($main)){[void][BbsUiProbe]::PostMessage($main,0x803C,[IntPtr]::Zero,[IntPtr]::Zero)}
                if($proc.WaitForExit(5000)){$cleanup='safe-shutdown'}else{
                    Assert-Probe $false 'safe-shutdown-timeout' @{pid=$proc.Id;forceCleanupAllowed=[bool]$KillOwnedOnTimeout}
                    if($KillOwnedOnTimeout){$proc.Kill();[void]$proc.WaitForExit(3000);$cleanup='explicit-owned-timeout-force-cleanup'}else{$cleanup='owned-smoke-left-to-auto-exit-at-120s'}
                }
            }else{$cleanup='already-exited'}
            if($proc.HasExited -and -not $Keep -and (Test-Path -LiteralPath $runtimeRoot)){
                $verified=[IO.Path]::GetFullPath($runtimeRoot)
                if($verified -ne (Join-Path $OutputDir 'isolated-runtime') -or -not $verified.StartsWith($tempPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Runtime cleanup path verification failed'}
                Remove-Item -LiteralPath $verified -Recurse -Force
            }
        }catch{Assert-Probe $false 'cleanup-exception' $_.Exception.ToString()}
    }
    $failed=@($checks | Where-Object {-not $_.pass})
    if($coverage -eq 'running'){$coverage='executed'}
    $report=[ordered]@{executable=$Executable;dpi=$Dpi;lightTheme=[bool]$Light;coverage=$coverage;coverageReason=$coverageReason;chosenLogicalSize=$chosenSize;pid=$(if($proc){$proc.Id}else{$null});startedUtc=$started.ToString('o');finishedUtc=[DateTime]::UtcNow.ToString('o');passed=($failed.Count -eq 0);assertionCount=$checks.Count;failureCount=$failed.Count;cleanup=$cleanup;footerNoticeMask=$false;logWheelFixture=[bool]$TestLogWheel;captureMethod='physical CopyFromScreen of own visible client/popup only; never WM_PRINT or PrintWindow';limitations=@('Sampled two-frame stability cannot establish absence of inter-frame flicker.','Text edge transitions test nonblank raster content, not text correctness.','Occlusion, offscreen windows, or unavailable interactive desktop cause explicit failure.','Header comparison excludes caption y<56 and sidebar x<224; the entire footer is compared without masking.');checks=$checks.ToArray();regions=$regionSamples.ToArray();fatal=$fatal}
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDir 'report.json') -Encoding UTF8
    Write-Output ("Assertions={0} Failures={1} Report={2}" -f $checks.Count,$failed.Count,(Join-Path $OutputDir 'report.json'))
}
if($coverage -eq 'not-covered'){exit 2}
if(@($checks | Where-Object {-not $_.pass}).Count -gt 0){exit 1}
exit 0
