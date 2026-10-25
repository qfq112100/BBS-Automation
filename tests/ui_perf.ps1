#requires -Version 5.1
<# Isolated synchronous paint smoke microbenchmark, not FPS, GPU/present latency or a skin correctness test.
Run IDENTICAL script/parameters against both immutable executables. No speed claim without matched measurements.
Only the launched smoke PID and its LOLBackupV3.NativeWindow tree are addressed. No global input or force kill.
Each process auto-expires after 120 seconds; normal cleanup posts WM_APP+60. Runtime fixtures are retained in TEMP.
#>
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Executable,
 [Parameter(Mandatory=$true)][string]$OutputDir,
 [ValidateRange(1,3)][int]$Runs=3,
 [ValidateRange(10,300)][int]$Iterations=120,
 [ValidateRange(1,50)][int]$Warmup=20
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2.0
$Executable=[IO.Path]::GetFullPath($Executable)
if(-not (Test-Path -LiteralPath $Executable -PathType Leaf)){throw 'Executable missing'}
$tempPrefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
if(-not $OutputDir.StartsWith($tempPrefix,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $OutputDir)){throw 'OutputDir must be fresh beneath TEMP'}
[void](New-Item -ItemType Directory -Path $OutputDir)
$source=@'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;
using System.Threading;
using System.Windows.Forms;
public static class BbsPerf {
 [StructLayout(LayoutKind.Sequential)] public struct R {public int L,T,X,Y;public R(int l,int t,int r,int b){L=l;T=t;X=r;Y=b;}}
 [StructLayout(LayoutKind.Sequential)] public struct P {public int X,Y; public P(int x,int y){X=x;Y=y;}}
 public delegate bool EnumProc(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr p);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
 [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h,out R r);
 [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h,ref P p);
 [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(P p);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr z,int x,int y,int w,int he,uint f);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] static extern bool InvalidateRect(IntPtr h,ref R r,bool erase);
 [DllImport("user32.dll")] static extern bool UpdateWindow(IntPtr h);
 [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",SetLastError=true)] static extern IntPtr SMT(IntPtr h,uint m,IntPtr w,IntPtr l,uint flags,uint ms,out IntPtr result);
 [DllImport("user32.dll",EntryPoint="SendMessageTimeoutW",CharSet=CharSet.Unicode)] static extern IntPtr SMTText(IntPtr h,uint m,IntPtr w,string text,uint flags,uint ms,out IntPtr result);
 [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process,uint kind);
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenThread(uint access,bool inherit,uint tid);
 [DllImport("kernel32.dll")] static extern bool GetThreadTimes(IntPtr h,out long created,out long exited,out long kernel,out long user);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long t);
 [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long t);
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
 public static void Physical(){try{SetThreadDpiAwarenessContext(new IntPtr(-4));}catch(EntryPointNotFoundException){SetProcessDPIAware();}}
 public static R Work(){var r=Screen.PrimaryScreen.WorkingArea;return new R(r.Left,r.Top,r.Right,r.Bottom);}
 public static IntPtr Find(uint pid){IntPtr result=IntPtr.Zero;EnumWindows(delegate(IntPtr h,IntPtr p){uint id;GetWindowThreadProcessId(h,out id);var s=new StringBuilder(256);GetClassName(h,s,256);if(id==pid&&s.ToString()=="LOLBackupV3.NativeWindow"&&IsWindowVisible(h)){result=h;return false;}return true;},IntPtr.Zero);return result;}
 static void Own(IntPtr h,uint pid){uint id;GetWindowThreadProcessId(h,out id);if(id!=pid)throw new Exception("Foreign/expired HWND rejected");}
 public static IntPtr Child(IntPtr root,uint pid,int ctrl){Own(root,pid);IntPtr result=IntPtr.Zero;EnumChildWindows(root,delegate(IntPtr h,IntPtr p){uint id;GetWindowThreadProcessId(h,out id);if(id==pid&&GetDlgCtrlID(h)==ctrl){result=h;return false;}return true;},IntPtr.Zero);if(result==IntPtr.Zero)throw new Exception("Missing log edit 303");return result;}
 public static long Send(IntPtr h,uint pid,uint msg,long w,long l){Own(h,pid);IntPtr result;if(SMT(h,msg,new IntPtr(w),new IntPtr(l),2,1500,out result)==IntPtr.Zero)throw new Exception("Owned send timeout "+msg);return result.ToInt64();}
 public static void Text(IntPtr h,uint pid,string s){Own(h,pid);IntPtr r;if(SMTText(h,12,IntPtr.Zero,s,2,1500,out r)==IntPtr.Zero||r==IntPtr.Zero)throw new Exception("Fixture failed");}
 public static void Shutdown(IntPtr h,uint pid){Own(h,pid);if(!PostMessage(h,0x803c,IntPtr.Zero,IntPtr.Zero))throw new Exception("Shutdown post failed");}
 public static R Client(IntPtr h){R r;if(!GetClientRect(h,out r))throw new Exception("Client rect failed");return r;}
 public static void Visible(IntPtr root,uint pid){Own(root,pid);if(!IsWindowVisible(root)||IsIconic(root))throw new Exception("Not visible");var r=Client(root);if(r.X!=983||r.Y!=727)throw new Exception("Expected exact 983x727 client, got "+r.X+"x"+r.Y);var origin=new P(0,0);ClientToScreen(root,ref origin);var work=Work();if(origin.X<work.L||origin.Y<work.T||origin.X+r.X>work.X||origin.Y+r.Y>work.Y)throw new Exception("Offscreen client");foreach(int x in new int[]{8,224,492,974})foreach(int y in new int[]{8,100,360,718}){uint id;GetWindowThreadProcessId(WindowFromPoint(new P(origin.X+x,origin.Y+y)),out id);if(id!=pid)throw new Exception("Occluded sample: refusing performance result");}}
 static long Cpu(IntPtr thread){long c,e,k,u;if(!GetThreadTimes(thread,out c,out e,out k,out u))throw new Exception("GetThreadTimes failed");return k+u;}
 public class Metric {public string scenario; public int samples;public double meanMs,p95Ms,uiCpuMs; public uint gdiBefore,gdiAfter,userBefore,userAfter; public long statsBefore,statsAfter;public long[] countersBefore,countersAfter;public int firstVisibleBefore,firstVisibleAfter;}
 static long[] Counters(IntPtr root,uint pid,long supported){if(supported!=1)return null;var a=new long[7];for(int i=0;i<a.Length;i++)a[i]=Send(root,pid,0x8043,i+1,0x55495046);return a;}
 public static Metric Measure(IntPtr root,IntPtr edit,uint pid,IntPtr process,string name,int warm,int count){
  Visible(root,pid);uint owner;uint tid=GetWindowThreadProcessId(root,out owner);IntPtr thread=OpenThread(0x40,false,tid);if(thread==IntPtr.Zero)throw new Exception("OpenThread failed");
  try{bool wheel=name=="logEditWheelPair"; R rect=name=="headerStrip"?new R(224,56,983,160):name=="footerStrip"?new R(224,679,983,727):new R(0,0,983,727);
   Action action=delegate {if(wheel){Send(edit,pid,0x20a,-120L*65536,0);UpdateWindow(edit);UpdateWindow(root);Send(edit,pid,0x20a,120L*65536,0);UpdateWindow(edit);UpdateWindow(root);}else{if(!InvalidateRect(root,ref rect,false))throw new Exception("InvalidateRect failed");UpdateWindow(root);}};
   for(int i=0;i<warm;i++)action(); Send(root,pid,0,0,0);Visible(root,pid);
   var m=new Metric();m.scenario=name;m.samples=count;m.gdiBefore=GetGuiResources(process,0);m.userBefore=GetGuiResources(process,1);m.statsBefore=Send(root,pid,0x8043,99,0x55495046);m.countersBefore=Counters(root,pid,m.statsBefore);m.firstVisibleBefore=wheel?(int)Send(edit,pid,0xce,0,0):-1;
   long freq;QueryPerformanceFrequency(out freq);var times=new double[count];long cpu=Cpu(thread);
   for(int i=0;i<count;i++){long a,b;QueryPerformanceCounter(out a);action();QueryPerformanceCounter(out b);times[i]=(b-a)*1000.0/freq;}
   m.uiCpuMs=(Cpu(thread)-cpu)/10000.0;m.firstVisibleAfter=wheel?(int)Send(edit,pid,0xce,0,0):-1;m.statsAfter=Send(root,pid,0x8043,99,0x55495046);m.countersAfter=Counters(root,pid,m.statsAfter);m.gdiAfter=GetGuiResources(process,0);m.userAfter=GetGuiResources(process,1);Visible(root,pid);
   if(wheel&&m.firstVisibleBefore!=m.firstVisibleAfter)throw new Exception("Wheel pair not repeatable");double total=0;foreach(double t in times)total+=t;Array.Sort(times);m.meanMs=total/count;m.p95Ms=times[(int)Math.Ceiling(count*.95)-1];return m;
  }finally{CloseHandle(thread);}
 }
}
'@
Add-Type -TypeDefinition $source -ReferencedAssemblies System.Drawing,System.Windows.Forms
[BbsPerf]::Physical()
$results=New-Object 'System.Collections.Generic.List[object]'
$fatal=$null
$hash=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
$scriptHash=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
$started=[DateTime]::UtcNow
try {
 $work=[BbsPerf]::Work()
 if($work.X-$work.L -lt 999 -or $work.Y-$work.T -lt 743){throw 'Fixed benchmark size cannot fit work area; no smaller fallback'}
 for($run=1;$run -le $Runs;$run++){
  $proc=$null;$root=[IntPtr]::Zero;$cleanup='not-started';$metrics=New-Object 'System.Collections.Generic.List[object]'
  $runtime=Join-Path $OutputDir ("run-$run-isolated-runtime")
  $argsText='--smoke "'+$runtime+'" --smoke-page 4 --smoke-size 983 727 --smoke-duration 120000 --smoke-dpi 96'
  try {
   $proc=Start-Process -FilePath $Executable -ArgumentList $argsText -PassThru
   $deadline=[DateTime]::UtcNow.AddSeconds(10)
   do{$root=[BbsPerf]::Find([uint32]$proc.Id);if($root -ne [IntPtr]::Zero){break};Start-Sleep -Milliseconds 30}while([DateTime]::UtcNow -lt $deadline -and -not $proc.HasExited)
   if($root -eq [IntPtr]::Zero){throw 'Owned visible root not found'}
   [void][BbsPerf]::ShowWindow($root,9)
   [void][BbsPerf]::SetWindowPos($root,[IntPtr](-1),$work.L+8,$work.T+8,983,727,0x40)
   Start-Sleep -Milliseconds 350
   [BbsPerf]::Visible($root,[uint32]$proc.Id)
   foreach($name in @('fullFrame','headerStrip','footerStrip')){$metrics.Add([BbsPerf]::Measure($root,[IntPtr]::Zero,[uint32]$proc.Id,$proc.Handle,$name,$Warmup,$Iterations))}
   [void][BbsPerf]::Send($root,[uint32]$proc.Id,0x111,102,0)
   $edit=[BbsPerf]::Child($root,[uint32]$proc.Id,303)
   # Construct CRLF explicitly so the fixture is invariant under file line-ending conversion.
   $fixture=((0..99|ForEach-Object {'UI wheel fixture line {0:d3}' -f $_}) -join ([string][char]13+[char]10))
   [BbsPerf]::Text($edit,[uint32]$proc.Id,$fixture)
   [void][BbsPerf]::Send($edit,[uint32]$proc.Id,0xb6,0,-10000)
   [void][BbsPerf]::Send($edit,[uint32]$proc.Id,0x20a,-120*65536,0)
   if([BbsPerf]::Send($edit,[uint32]$proc.Id,0xce,0,0) -le 0){throw 'Fixture failed to scroll'}
   [void][BbsPerf]::Send($edit,[uint32]$proc.Id,0xb6,0,-10000)
   $metrics.Add([BbsPerf]::Measure($root,$edit,[uint32]$proc.Id,$proc.Handle,'logEditWheelPair',$Warmup,$Iterations))
  }finally{
   if($proc){if(-not $proc.HasExited){if($root -ne [IntPtr]::Zero){[BbsPerf]::Shutdown($root,[uint32]$proc.Id)};if($proc.WaitForExit(5000)){$cleanup='safe-shutdown'}else{$cleanup='auto-exit-pending-120s';throw 'Safe shutdown timed out; never force-killed'}}else{$cleanup='already-exited'}}
   $results.Add([pscustomobject]@{run=$run;pid=$(if($proc){$proc.Id}else{$null});args=$argsText;cleanup=$cleanup;metrics=$metrics.ToArray()})
  }
 }
}catch{$fatal=$_.Exception.ToString()}
$report=[ordered]@{schema=1;passed=($null -eq $fatal);executable=$Executable;sha256=$hash;harnessSha256=$scriptHash;startedUtc=$started.ToString('o');finishedUtc=[DateTime]::UtcNow.ToString('o');parameters=@{runs=$Runs;iterations=$Iterations;warmup=$Warmup;logicalDpi=96;clientWidth=983;clientHeight=727;theme='default dark';fixtureLines=100;wheelDeltas=@(-120,120);topmostOwned=$true};method='External QPC per synchronous InvalidateRect(false)+UpdateWindow (parent strips); wheel pair SendMessageTimeout plus UpdateWindow(edit)+UpdateWindow(parent) after each delta. CPU is root UI THREAD GetThreadTimes kernel+user, not process CPU.';limitations=@('No skin correctness, flicker, FPS, GPU or compositor-present claim.','Sampled owned WindowFromPoint occlusion checks cannot prove zero inter-sample occlusion.','Optional WM_APP+67 lp0x55495046: wp99 handshake 1 means supported, else unavailable. countersBefore/After query wp1..7 outside timing: paintCount,textDrawCount,textSkipCount,logoDrawCount,uiDirProbes,paintDirtyPixels,layoutCount. Never resets counters.','Handle counts are process GDI/USER resources; CPU is UI thread only.','Same frozen harness and parameters required for after comparison; timer/desktop noise remains.','Runtime fixtures retained only beneath fresh TEMP; no real configuration/service/backup/apps touched.');runs=$results.ToArray();fatal=$fatal}
$report|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDir 'perf.json') -Encoding UTF8
Write-Output (Join-Path $OutputDir 'perf.json')
if($fatal){Write-Error $fatal;exit 1}
exit 0
