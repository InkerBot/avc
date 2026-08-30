#define AppName "AVC"
#define AppVersion "0.1.0"
#define StageDir "..\dist\avc-stage-0.1.0"

[Setup]
AppId=AVC.AudioGraph.Desktop
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=AVC
DefaultDirName={autopf}\AVC
DefaultGroupName=AVC
OutputDir=..\dist\installer
OutputBaseFilename=AVC-{#AppVersion}-x64-setup
SetupArchitecture=x64
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
WizardStyle=modern
CloseApplications=yes
UninstallDisplayIcon={app}\avc.exe
VersionInfoVersion=0.1.0.0
VersionInfoProductVersion=0.1.0.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "{#StageDir}\bin\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\share\avc\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "redist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "redist\MicrosoftEdgeWebview2Setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Icons]
Name: "{autoprograms}\AVC"; Filename: "{app}\avc.exe"
Name: "{autodesktop}\AVC"; Filename: "{app}\avc.exe"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "正在安装 Visual C++ 运行库……"; Flags: waituntilterminated runhidden
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "正在安装 WebView2 Runtime……"; Flags: waituntilterminated runhidden
Filename: "{app}\avc.exe"; Description: "启动 AVC"; Flags: nowait postinstall skipifsilent
