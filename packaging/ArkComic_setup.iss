; Ark Comic — Inno Setup 安装脚本
; 功能：简体中文界面 / 自定义安装路径 / 干净卸载（删配置 + 取消文件关联）
; 用法：ISCC.exe "packaging\ArkComic_setup.iss"

#define MyAppName "Ark Comic"
#define MyAppVersion "1.1.0"
#define MyAppPublisher "Ark Comic Contributors"
#define MyAppExeName "ArkComic.exe"
#define MyAppId "{8D4C6F42-5B9E-4A3C-9D2F-1E7B8A0C4D5F}"

[Setup]
AppId={{#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
; 安装包文件属性版本（AppVersion 只写安装元数据，PE 版本资源需显式指定）
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\ArkComic
DefaultGroupName={#MyAppName}
DisableDirPage=no
; 文件关联写 HKCU（Software\Classes），无需管理员
PrivilegesRequired=lowest
LanguageDetectionMethod=uilanguage
ShowLanguageDialog=no
; 安装包图标（软件 logo）
SetupIconFile=..\resources\app.ico
; 输出
OutputBaseFilename=ArkComic_Setup
OutputDir=..\release_pkg
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
; 主程序
Source: "..\build\ArkComic.exe"; DestDir: "{app}"; Flags: ignoreversion
; 解码 DLL（运行时必需）
Source: "..\build\turbojpeg.dll"; DestDir: "{app}"; Flags: ignoreversion
; N 卡硬解（可选，无 N 卡自动回退 CPU）
Source: "..\build\nvjpeg64_12.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\cudart64_12.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 文件关联功能已移除（避免抢占 .zip/.cbz 的通用关联）

[UninstallDelete]
; 删除配置目录（%LOCALAPPDATA%\ArkComic）
Type: filesandordirs; Name: "{localappdata}\ArkComic"

[Code]
// 卸载钩子：确认弹窗 + 安全删除安装目录（目录名必须是 ArkComic，防止误删父目录）
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir, DirName: String;
begin
  if CurUninstallStep = usUninstall then
    if MsgBox('卸载将删除软件及全部配置/日志，确定继续？', mbConfirmation, MB_YESNO) = IDNO then
      Abort;
  if CurUninstallStep = usPostUninstall then
  begin
    AppDir := ExpandConstant('{app}');
    DirName := ExtractFileName(RemoveBackslash(AppDir));
    if Pos('ArkComic', DirName) > 0 then
      DelTree(AppDir, True, True, True)
    else
      MsgBox('安装目录不是 ArkComic 命名（' + DirName + '），为安全起见未删除整个文件夹，请手动清理。',
        mbInformation, MB_OK);
  end;
end;
