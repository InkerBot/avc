#define ExtensionName "AVC RVC Extension"
#define ExtensionVersion "0.1.0"
#define StageDir "..\dist\rvc-extension-stage-0.1.0"

[Setup]
AppId=AVC.Extension.RVC
AppName={#ExtensionName}
AppVersion={#ExtensionVersion}
AppPublisher=AVC
DefaultDirName={localappdata}\Programs\AVC RVC Extension
DisableDirPage=yes
OutputDir=..\dist\installer
OutputBaseFilename=AVC-RVC-Extension-{#ExtensionVersion}-x64-setup
SetupArchitecture=x64
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=yes
VersionInfoVersion=0.1.0.0
VersionInfoProductVersion=0.1.0.0
UninstallDisplayName=AVC RVC Extension

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[CustomMessages]
english.AvcNotFound=AVC 0.1.0 was not detected. You can install the extension now, but it will not be usable until the matching AVC version is installed. Continue?
english.RestartAvc=RVC was installed. Completely exit and restart AVC, then confirm that RVC is enabled in the Extensions panel.
chinesesimplified.AvcNotFound=未检测到 AVC 0.1.0。可以先安装扩展，但安装匹配版本的 AVC 后才能使用。是否继续？
chinesesimplified.RestartAvc=RVC 已安装。请完整退出并重新启动 AVC，然后在“扩展”面板中确认 RVC 已启用。

[Files]
Source: "{#StageDir}\bin\extensions\avc-rvc.dll"; DestDir: "{localappdata}\avc\extensions"; Flags: ignoreversion
Source: "{#StageDir}\bin\extensions\lib\*"; DestDir: "{localappdata}\avc\extensions\lib"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\libexec\avc\rvc-converter\*"; DestDir: "{localappdata}\avc\rvc-converter"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\share\doc\avc\third-party\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "AVC_RVC_CONVERTER_ROOT"; ValueData: "{localappdata}\avc\rvc-converter"; Flags: preservestringtype; BeforeInstall: SavePreviousConverterRoot

[Code]
const
  InstallerStateKey = 'Software\AVC\RVC Extension Installer';

procedure SavePreviousConverterRoot();
var
  ExistingRoot: String;
  StateSaved: Cardinal;
begin
  if RegQueryDWordValue(HKCU, InstallerStateKey, 'StateSaved', StateSaved) and
     (StateSaved = 1) then
    Exit;

  if RegQueryStringValue(HKCU, 'Environment', 'AVC_RVC_CONVERTER_ROOT', ExistingRoot) then
  begin
    RegWriteDWordValue(HKCU, InstallerStateKey, 'HadPreviousValue', 1);
    RegWriteStringValue(HKCU, InstallerStateKey, 'PreviousValue', ExistingRoot);
  end
  else
    RegWriteDWordValue(HKCU, InstallerStateKey, 'HadPreviousValue', 0);

  RegWriteDWordValue(HKCU, InstallerStateKey, 'StateSaved', 1);
end;

function AvcInstalled(): Boolean;
var
  InstallLocation: String;
begin
  Result :=
    (RegQueryStringValue(HKLM64,
       'Software\Microsoft\Windows\CurrentVersion\Uninstall\AVC.AudioGraph.Desktop_is1',
       'InstallLocation', InstallLocation) and
     FileExists(AddBackslash(InstallLocation) + 'avc.exe')) or
    (RegQueryStringValue(HKCU64,
       'Software\Microsoft\Windows\CurrentVersion\Uninstall\AVC.AudioGraph.Desktop_is1',
       'InstallLocation', InstallLocation) and
     FileExists(AddBackslash(InstallLocation) + 'avc.exe')) or
    FileExists(ExpandConstant('{autopf}\AVC\avc.exe'));
end;

function InitializeSetup(): Boolean;
begin
  Result := AvcInstalled();
  if not Result then
    Result := MsgBox(ExpandConstant('{cm:AvcNotFound}'), mbConfirmation, MB_YESNO) = IDYES;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    MsgBox(ExpandConstant('{cm:RestartAvc}'), mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  CurrentRoot: String;
  PreviousRoot: String;
  HadPreviousValue: Cardinal;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;

  if RegQueryStringValue(HKCU, 'Environment', 'AVC_RVC_CONVERTER_ROOT', CurrentRoot) and
     (CompareText(CurrentRoot, ExpandConstant('{localappdata}\avc\rvc-converter')) = 0) then
  begin
    if RegQueryDWordValue(HKCU, InstallerStateKey, 'HadPreviousValue', HadPreviousValue) and
       (HadPreviousValue = 1) and
       RegQueryStringValue(HKCU, InstallerStateKey, 'PreviousValue', PreviousRoot) then
      RegWriteExpandStringValue(HKCU, 'Environment', 'AVC_RVC_CONVERTER_ROOT', PreviousRoot)
    else
      RegDeleteValue(HKCU, 'Environment', 'AVC_RVC_CONVERTER_ROOT');
  end;

  RegDeleteValue(HKCU, InstallerStateKey, 'PreviousValue');
  RegDeleteValue(HKCU, InstallerStateKey, 'HadPreviousValue');
  RegDeleteValue(HKCU, InstallerStateKey, 'StateSaved');
  RegDeleteKeyIfEmpty(HKCU, InstallerStateKey);
end;
