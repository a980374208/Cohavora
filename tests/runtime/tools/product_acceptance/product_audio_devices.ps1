param(
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{32}$')][string]$RunId
)
$ErrorActionPreference='Stop'
if (Test-Path -LiteralPath $Output) { throw 'Evidence destination must be new' }
Add-Type @'
using System;
using System.Runtime.InteropServices;
[ComImport,Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] public class MMDeviceEnumerator {}
[ComImport,Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] public interface IMMDeviceEnumerator {
 [PreserveSig] int EnumAudioEndpoints(int flow,uint states,out IMMDeviceCollection collection);
 [PreserveSig] int GetDefaultAudioEndpoint(int flow,int role,out IntPtr device);
}
[ComImport,Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] public interface IMMDeviceCollection {
 [PreserveSig] int GetCount(out uint count);
}
public static class EndpointWitness {
 public static string Read() {
  var e=(IMMDeviceEnumerator)new MMDeviceEnumerator();
  IMMDeviceCollection c=null; IntPtr d=IntPtr.Zero;
  try {
   uint count=0; int hr=e.EnumAudioEndpoints(1,1,out c);
   if(hr<0)Marshal.ThrowExceptionForHR(hr);
   Marshal.ThrowExceptionForHR(c.GetCount(out count));
   int def=e.GetDefaultAudioEndpoint(1,0,out d);
   return "{\"active_capture_endpoints\":"+count+",\"enumeration_hresult\":\""+hr.ToString("X8")+"\",\"default_capture_hresult\":\""+def.ToString("X8")+"\"}";
  } finally {
   if(d!=IntPtr.Zero)Marshal.Release(d);
   if(c!=null)Marshal.ReleaseComObject(c);
   Marshal.ReleaseComObject(e);
  }
 }
}
'@
$result=[EndpointWitness]::Read() | ConvertFrom-Json
$result | Add-Member -NotePropertyName run_id -NotePropertyValue $RunId
$result | Add-Member -NotePropertyName utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
$result | Add-Member -NotePropertyName collector -NotePropertyValue 'independent_windows_mmdevice'
$result | ConvertTo-Json | Set-Content -LiteralPath $Output -Encoding UTF8
