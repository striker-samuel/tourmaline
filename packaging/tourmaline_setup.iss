#define AppName "Tourmaline"
#ifndef AppVersion
  #define AppVersion "0.1"
#endif
#ifndef StageDir
  #error "StageDir must point to a built setup payload."
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{86F34108-8BF5-4555-B396-B3C76844B35D}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Tourmaline
DefaultDirName={autopf}\Tourmaline
DefaultGroupName=Tourmaline
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=TourmalineSetup-Windows
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\TourmalineSetupWizard.exe

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Tourmaline Setup Wizard"; Filename: "{app}\TourmalineSetupWizard.exe"

[Run]
Filename: "{app}\TourmalineSetupWizard.exe"; Description: "Create a Tourmaline project"; Flags: nowait postinstall skipifsilent runasoriginaluser
