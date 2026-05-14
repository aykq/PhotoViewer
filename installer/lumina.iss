#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define AppName           "Lumina"
#define AppPublisher      "Lumina"
#define AppURL            "https://github.com/aykq/Lumina"
#define AppExeName        "LuminaCpp.exe"
#define AppDllName        "LuminaShell.dll"
#define AppId             "{{4B8E9F2D-1A3C-4D6E-8F9A-1B2C3D4E5F6A}"
#define ClsidLuminaShell  "3C7F5A2D-4B8E-4F5C-A1B2-3C4D5E6F7A8B"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
OutputDir=output
OutputBaseFilename=Lumina-{#AppVersion}-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
MinVersion=10.0
ChangesAssociations=yes
UninstallDisplayIcon={app}\{#AppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "shortcut_desktop";    Description: "Create a &desktop shortcut";      GroupDescription: "Additional shortcuts:"; Flags: checkedonce
Name: "shortcut_startmenu";  Description: "Create a &Start Menu shortcut";   GroupDescription: "Additional shortcuts:"; Flags: checkedonce

Name: "assoc\jpg";   Description: "JPEG (.jpg, .jpeg)";          GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\png";   Description: "PNG (.png)";                  GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\gif";   Description: "GIF (.gif)";                  GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\bmp";   Description: "BMP (.bmp)";                  GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\tiff";  Description: "TIFF (.tiff, .tif)";          GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\ico";   Description: "Icon (.ico)";                 GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\webp";  Description: "WebP (.webp)";                GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\heic";  Description: "HEIC / HEIF (.heic, .heif)";  GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\avif";  Description: "AVIF (.avif)";                GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce
Name: "assoc\jxl";   Description: "JPEG XL (.jxl)";             GroupDescription: "Associate image formats with Lumina:"; Flags: checkedonce

[Files]
Source: "..\x64\Release\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\x64\Release\{#AppDllName}"; DestDir: "{app}"; Flags: ignoreversion regserver

[Icons]
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: shortcut_desktop
Name: "{group}\{#AppName}";       Filename: "{app}\{#AppExeName}"; Tasks: shortcut_startmenu

[Registry]
; ──────────────────────────────────────────────────────────────────────────────
; JPEG (.jpg, .jpeg)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JPG";                        ValueType: string; ValueName: "";                 ValueData: "JPEG Image";              Flags: uninsdeletekey;   Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JPG\DefaultIcon";            ValueType: string; ValueName: "";                 ValueData: "{app}\{#AppExeName},0";   Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JPG\shell\open";             ValueType: string; ValueName: "FriendlyAppName";  ValueData: "{#AppName}";              Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JPG\shell\open\command";     ValueType: string; ValueName: "";                 ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Classes\.jpg\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.JPG"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Classes\.jpeg\OpenWithProgids";                   ValueType: string; ValueName: "Lumina.Image.JPG"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\jpg

; ──────────────────────────────────────────────────────────────────────────────
; PNG (.png)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.PNG";                        ValueType: string; ValueName: "";                 ValueData: "PNG Image";               Flags: uninsdeletekey;   Tasks: assoc\png
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.PNG\DefaultIcon";            ValueType: string; ValueName: "";                 ValueData: "{app}\{#AppExeName},0";   Tasks: assoc\png
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.PNG\shell\open";             ValueType: string; ValueName: "FriendlyAppName";  ValueData: "{#AppName}";              Tasks: assoc\png
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.PNG\shell\open\command";     ValueType: string; ValueName: "";                 ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\png
Root: HKLM; Subkey: "SOFTWARE\Classes\.png\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.PNG"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\png

; ──────────────────────────────────────────────────────────────────────────────
; GIF (.gif)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.GIF";                        ValueType: string; ValueName: "";                 ValueData: "GIF Image";               Flags: uninsdeletekey;   Tasks: assoc\gif
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.GIF\DefaultIcon";            ValueType: string; ValueName: "";                 ValueData: "{app}\{#AppExeName},0";   Tasks: assoc\gif
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.GIF\shell\open";             ValueType: string; ValueName: "FriendlyAppName";  ValueData: "{#AppName}";              Tasks: assoc\gif
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.GIF\shell\open\command";     ValueType: string; ValueName: "";                 ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\gif
Root: HKLM; Subkey: "SOFTWARE\Classes\.gif\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.GIF"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\gif

; ──────────────────────────────────────────────────────────────────────────────
; BMP (.bmp)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.BMP";                        ValueType: string; ValueName: "";                 ValueData: "Bitmap Image";            Flags: uninsdeletekey;   Tasks: assoc\bmp
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.BMP\DefaultIcon";            ValueType: string; ValueName: "";                 ValueData: "{app}\{#AppExeName},0";   Tasks: assoc\bmp
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.BMP\shell\open";             ValueType: string; ValueName: "FriendlyAppName";  ValueData: "{#AppName}";              Tasks: assoc\bmp
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.BMP\shell\open\command";     ValueType: string; ValueName: "";                 ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\bmp
Root: HKLM; Subkey: "SOFTWARE\Classes\.bmp\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.BMP"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\bmp

; ──────────────────────────────────────────────────────────────────────────────
; TIFF (.tiff, .tif)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.TIFF";                       ValueType: string; ValueName: "";                  ValueData: "TIFF Image";             Flags: uninsdeletekey;   Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.TIFF\DefaultIcon";           ValueType: string; ValueName: "";                  ValueData: "{app}\{#AppExeName},0";  Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.TIFF\shell\open";            ValueType: string; ValueName: "FriendlyAppName";   ValueData: "{#AppName}";             Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.TIFF\shell\open\command";    ValueType: string; ValueName: "";                  ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Classes\.tiff\OpenWithProgids";                   ValueType: string; ValueName: "Lumina.Image.TIFF"; ValueData: "";                       Flags: uninsdeletevalue; Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Classes\.tif\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.TIFF"; ValueData: "";                       Flags: uninsdeletevalue; Tasks: assoc\tiff

; ──────────────────────────────────────────────────────────────────────────────
; ICO (.ico)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.ICO";                        ValueType: string; ValueName: "";                 ValueData: "Icon File";               Flags: uninsdeletekey;   Tasks: assoc\ico
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.ICO\DefaultIcon";            ValueType: string; ValueName: "";                 ValueData: "{app}\{#AppExeName},0";   Tasks: assoc\ico
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.ICO\shell\open";             ValueType: string; ValueName: "FriendlyAppName";  ValueData: "{#AppName}";              Tasks: assoc\ico
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.ICO\shell\open\command";     ValueType: string; ValueName: "";                 ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\ico
Root: HKLM; Subkey: "SOFTWARE\Classes\.ico\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.ICO"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\ico

; ──────────────────────────────────────────────────────────────────────────────
; WebP (.webp)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.WEBP";                       ValueType: string; ValueName: "";                  ValueData: "WebP Image";             Flags: uninsdeletekey;   Tasks: assoc\webp
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.WEBP\DefaultIcon";           ValueType: string; ValueName: "";                  ValueData: "{app}\{#AppExeName},0";  Tasks: assoc\webp
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.WEBP\shell\open";            ValueType: string; ValueName: "FriendlyAppName";   ValueData: "{#AppName}";             Tasks: assoc\webp
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.WEBP\shell\open\command";    ValueType: string; ValueName: "";                  ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\webp
Root: HKLM; Subkey: "SOFTWARE\Classes\.webp\OpenWithProgids";                   ValueType: string; ValueName: "Lumina.Image.WEBP"; ValueData: "";                       Flags: uninsdeletevalue; Tasks: assoc\webp

; ──────────────────────────────────────────────────────────────────────────────
; HEIC / HEIF (.heic, .heif)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.HEIC";                       ValueType: string; ValueName: "";                  ValueData: "HEIC Image";             Flags: uninsdeletekey;   Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.HEIC\DefaultIcon";           ValueType: string; ValueName: "";                  ValueData: "{app}\{#AppExeName},0";  Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.HEIC\shell\open";            ValueType: string; ValueName: "FriendlyAppName";   ValueData: "{#AppName}";             Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.HEIC\shell\open\command";    ValueType: string; ValueName: "";                  ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Classes\.heic\OpenWithProgids";                   ValueType: string; ValueName: "Lumina.Image.HEIC"; ValueData: "";                       Flags: uninsdeletevalue; Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Classes\.heif\OpenWithProgids";                   ValueType: string; ValueName: "Lumina.Image.HEIC"; ValueData: "";                       Flags: uninsdeletevalue; Tasks: assoc\heic

; ──────────────────────────────────────────────────────────────────────────────
; AVIF (.avif)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.AVIF";                       ValueType: string; ValueName: "";                  ValueData: "AVIF Image";             Flags: uninsdeletekey;   Tasks: assoc\avif
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.AVIF\DefaultIcon";           ValueType: string; ValueName: "";                  ValueData: "{app}\{#AppExeName},0";  Tasks: assoc\avif
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.AVIF\shell\open";            ValueType: string; ValueName: "FriendlyAppName";   ValueData: "{#AppName}";             Tasks: assoc\avif
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.AVIF\shell\open\command";    ValueType: string; ValueName: "";                  ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\avif
Root: HKLM; Subkey: "SOFTWARE\Classes\.avif\OpenWithProgids";                   ValueType: string; ValueName: "Lumina.Image.AVIF"; ValueData: "";                       Flags: uninsdeletevalue; Tasks: assoc\avif

; ──────────────────────────────────────────────────────────────────────────────
; JPEG XL (.jxl)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JXL";                        ValueType: string; ValueName: "";                 ValueData: "JPEG XL Image";           Flags: uninsdeletekey;   Tasks: assoc\jxl
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JXL\DefaultIcon";            ValueType: string; ValueName: "";                 ValueData: "{app}\{#AppExeName},0";   Tasks: assoc\jxl
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JXL\shell\open";             ValueType: string; ValueName: "FriendlyAppName";  ValueData: "{#AppName}";              Tasks: assoc\jxl
Root: HKLM; Subkey: "SOFTWARE\Classes\Lumina.Image.JXL\shell\open\command";     ValueType: string; ValueName: "";                 ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: assoc\jxl
Root: HKLM; Subkey: "SOFTWARE\Classes\.jxl\OpenWithProgids";                    ValueType: string; ValueName: "Lumina.Image.JXL"; ValueData: "";                        Flags: uninsdeletevalue; Tasks: assoc\jxl

; ──────────────────────────────────────────────────────────────────────────────
; Registered Applications capability (for Default Apps settings)
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\RegisteredApplications"; ValueType: string; ValueName: "Lumina"; ValueData: "SOFTWARE\Lumina\Capabilities"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities";    ValueType: string; ValueName: "ApplicationName";        ValueData: "Lumina";                    Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities";    ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Fast native image viewer"
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg";  ValueData: "Lumina.Image.JPG";  Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "Lumina.Image.JPG";  Tasks: assoc\jpg
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png";  ValueData: "Lumina.Image.PNG";  Tasks: assoc\png
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".gif";  ValueData: "Lumina.Image.GIF";  Tasks: assoc\gif
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".bmp";  ValueData: "Lumina.Image.BMP";  Tasks: assoc\bmp
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "Lumina.Image.TIFF"; Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif";  ValueData: "Lumina.Image.TIFF"; Tasks: assoc\tiff
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ico";  ValueData: "Lumina.Image.ICO";  Tasks: assoc\ico
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "Lumina.Image.WEBP"; Tasks: assoc\webp
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heic"; ValueData: "Lumina.Image.HEIC"; Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heif"; ValueData: "Lumina.Image.HEIC"; Tasks: assoc\heic
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avif"; ValueData: "Lumina.Image.AVIF"; Tasks: assoc\avif
Root: HKLM; Subkey: "SOFTWARE\Lumina\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jxl";  ValueData: "Lumina.Image.JXL";  Tasks: assoc\jxl

; ──────────────────────────────────────────────────────────────────────────────
; Shell extension CLSID ACLs — prevent standard users from redirecting DLL path
; Applied after regserver calls DllRegisterServer(), which creates the keys.
; ──────────────────────────────────────────────────────────────────────────────
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{{#ClsidLuminaShell}}";                    Flags: uninsdeletekey; Permissions: admins-full system-full users-readexec
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{{{#ClsidLuminaShell}}\InprocServer32";     Permissions: admins-full system-full users-readexec

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if GetEnv('GITHUB_ACTIONS') <> 'true' then
  begin
    if MsgBox(
      'WARNING: Local Build' + #13#10 + #13#10 +
      'This installer is being compiled outside of the official CI pipeline.' + #13#10 +
      'Release installers must only be built via GitHub Actions to ensure' + #13#10 +
      'supply-chain integrity (SLSA provenance attestation, SHA-256 checksums).' + #13#10 + #13#10 +
      'Do NOT distribute this installer. Proceed only for local testing.' + #13#10 + #13#10 +
      'Continue anyway?',
      mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
    end;
  end;
end;
