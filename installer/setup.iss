; ============================================================================
;  Інсталятор програми «Монітор стану дисків»
;  Збирається в Inno Setup 6 (https://jrsoftware.org/isdl.php):
;  відкрийте цей файл в Inno Setup Compiler і натисніть Build -> Compile (Ctrl+F9).
;  Готовий інсталятор з'явиться в папці Output поруч із папкою installer.
; ============================================================================

#define MyAppName      "Монітор стану дисків"
#define MyAppVersion   "1.6"
#define MyAppPublisher "Кирилюк Микола"
#define MyAppExe       "DiskMonitor.exe"
; Іменований mutex, який створює запущена програма (див. kMutexName у main.cpp)
#define MyAppMutex     "DiskMonitorKursova_Mutex"

[Setup]
; Унікальний ідентифікатор програми. Не змінюйте його між версіями,
; інакше нова версія встановиться поруч зі старою замість оновлення.
AppId={{8C1B6D2E-4F3A-4B7E-9A51-2D6F0C3E7B19}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\DiskMonitor
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
ShowLanguageDialog=no

; Крок з ліцензійною угодою
LicenseFile=EULA.txt

; Потрібні права адміністратора: встановлення в Program Files і створення завдання автозапуску
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; Чи запущена програма, перевіряється в розділі [Code] (InitializeSetup), а не директивою AppMutex:
; під час автоматичного оновлення інсталятор має дочекатися, поки програма сама закриється
CloseApplications=yes
; Налаштування програми в реєстрі (HKCU) видаляються разом із програмою
UsedUserAreasWarning=no

OutputDir=..\Output
OutputBaseFilename=DiskMonitor_Setup_{#MyAppVersion}
SetupIconFile=..\src\app.ico
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes

VersionInfoVersion=1.6.0.0
VersionInfoDescription=Інсталятор програми «{#MyAppName}»
VersionInfoProductName={#MyAppName}

[Languages]
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "desktopicon"; Description: "Створити ярлик на робочому столі"; GroupDescription: "Ярлики:"
Name: "autostart"; Description: "Запускати програму автоматично при старті Windows (згорнутою в трей біля годинника)"; GroupDescription: "Автозавантаження:"

[Files]
Source: "..\bin\{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "EULA.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{group}\Видалити {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Registry]
; Налаштування (перевірка оновлень) створює сама програма; тут лише видалення разом із програмою
Root: HKCU; Subkey: "Software\DiskMonitorKursova"; Flags: uninsdeletekey dontcreatekey

[UninstallDelete]
; Журнал подій і завантажені під час оновлень інсталятори. Видаляються лише конкретні файли програми,
; а не цілі папки: так деінсталятор з правами адміністратора не зачепить нічого зайвого, навіть якщо
; на місці папки виявиться посилання на іншу папку
Type: files; Name: "{localappdata}\DiskMonitor\events.log"
Type: dirifempty; Name: "{localappdata}\DiskMonitor"
Type: files; Name: "{%TEMP}\DiskMonitor_Update\DiskMonitor_Setup*.exe"
Type: dirifempty; Name: "{%TEMP}\DiskMonitor_Update"

[Run]
; Галочка на останній сторінці інсталятора
Filename: "{app}\{#MyAppExe}"; Description: "Запустити програму прямо зараз"; Flags: nowait postinstall skipifsilent runascurrentuser
; Після автоматичного оновлення (тихий режим) нова версія запускається сама
Filename: "{app}\{#MyAppExe}"; Parameters: "{code:RestartParams}"; Flags: nowait runascurrentuser; Check: IsUpdateMode

[Code]
// ---------------------------------------------------------------------------
// Автозапуск через Планувальник завдань Windows.
// Чому не звичайний запис у реєстрі (Run): програма з автозапуску з реєстру стартує
// без прав адміністратора, і частина SMART-даних буде недоступна. Завдання планувальника
// з рівнем "найвищі права" запускає програму з повними правами і без вікна UAC.
// Якщо створити завдання не вдасться, використовується реєстр як запасний варіант.
// ---------------------------------------------------------------------------
const
  AutostartName = 'DiskMonitorKursova';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

function XmlEscape(S: String): String;
begin
  Result := S;
  StringChangeEx(Result, '&', '&amp;', True);
  StringChangeEx(Result, '<', '&lt;', True);
  StringChangeEx(Result, '>', '&gt;', True);
end;

function RunSchtasks(Params: String): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\schtasks.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure RemoveAutostart;
begin
  RunSchtasks('/Delete /F /TN "' + AutostartName + '"');
  RegDeleteValue(HKLM, RunKey, AutostartName);
end;

procedure CreateAutostart;
var
  Xml: TArrayOfString;
  XmlFile, Exe: String;
begin
  Exe := ExpandConstant('{app}\{#MyAppExe}');
  XmlFile := ExpandConstant('{tmp}\autostart_task.xml');

  SetArrayLength(Xml, 1);
  Xml[0] :=
    '<?xml version="1.0" encoding="UTF-8"?>' + #13#10 +
    '<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">' + #13#10 +
    '  <RegistrationInfo><Description>Disk health monitor autostart (course project)</Description></RegistrationInfo>' + #13#10 +
    '  <Triggers>' + #13#10 +
    '    <LogonTrigger><Enabled>true</Enabled><Delay>PT15S</Delay></LogonTrigger>' + #13#10 +
    '  </Triggers>' + #13#10 +
    '  <Principals>' + #13#10 +
    '    <Principal id="Author">' + #13#10 +
    '      <GroupId>S-1-5-32-545</GroupId>' + #13#10 +
    '      <RunLevel>HighestAvailable</RunLevel>' + #13#10 +
    '    </Principal>' + #13#10 +
    '  </Principals>' + #13#10 +
    '  <Settings>' + #13#10 +
    '    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>' + #13#10 +
    '    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>' + #13#10 +
    '    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>' + #13#10 +
    '    <AllowStartOnDemand>true</AllowStartOnDemand>' + #13#10 +
    '    <Enabled>true</Enabled>' + #13#10 +
    '    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>' + #13#10 +
    '    <Priority>5</Priority>' + #13#10 +
    '  </Settings>' + #13#10 +
    '  <Actions Context="Author">' + #13#10 +
    '    <Exec><Command>' + XmlEscape(Exe) + '</Command><Arguments>/tray</Arguments></Exec>' + #13#10 +
    '  </Actions>' + #13#10 +
    '</Task>';

  if not (SaveStringsToUTF8File(XmlFile, Xml, False) and
          RunSchtasks('/Create /F /TN "' + AutostartName + '" /XML "' + XmlFile + '"')) then
    RegWriteStringValue(HKLM, RunKey, AutostartName, '"' + Exe + '" /tray');
end;

// ---------------------------------------------------------------------------
// Автоматичне оновлення.
// Програма завантажує новий інсталятор, запускає його з параметрами
// /SILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE [/TRAY] і одразу закривається.
// /UPDATE — дочекатися, поки стара копія програми закриється, і після встановлення запустити нову;
// /TRAY — програма до оновлення працювала згорнутою в трей, тож і нова запускається в треї.
// ---------------------------------------------------------------------------
function HasParam(Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
      Result := True;
end;

function IsUpdateMode: Boolean;
begin
  Result := HasParam('/UPDATE');
end;

function RestartParams(Param: String): String;
begin
  Result := '/noelevate';
  if HasParam('/TRAY') then
    Result := Result + ' /tray';
end;

// Чекає, поки програма закриється. Під час оновлення вона закривається сама (до 2 хв: стара
// версія чекає, поки фоновий потік завершить поточний запит WMI, а він може тривати довго),
// інакше користувача просять закрити її, як це робить стандартна перевірка AppMutex.
function WaitForAppClosed(Msg: String): Boolean;
var
  I: Integer;
begin
  if IsUpdateMode then
  begin
    Log('Автоматичне оновлення: чекаємо, поки програма закриється');
    I := 0;
    while CheckForMutexes('{#MyAppMutex}') and (I < 240) do
    begin
      Sleep(500);
      I := I + 1;
    end;
    Log(Format('Очікування тривало %d мс', [I * 500]));
    Sleep(1000);   // mutex зникає трохи раніше, ніж процес завершується повністю
  end;

  Result := True;
  while CheckForMutexes('{#MyAppMutex}') do
    if SuppressibleMsgBox(FmtMessage(Msg, ['{#MyAppName}']), mbError, MB_OKCANCEL, IDCANCEL) <> IDOK then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeSetup: Boolean;
begin
  Result := WaitForAppClosed(SetupMessage(msgSetupAppRunningError));
end;

function InitializeUninstall: Boolean;
begin
  Result := WaitForAppClosed(SetupMessage(msgUninstallAppRunningError));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    RemoveAutostart;                          // прибираємо старе налаштування (при перевстановленні)
    if WizardIsTaskSelected('autostart') then
      CreateAutostart;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveAutostart;
end;
