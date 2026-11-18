#!/usr/bin/env python3
"""Generate a git patch for one of the verified Ryubing and Kenji-NX revisions."""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[1]
PINS = {
    "ryubing": "6af327bdd50366163d80bc692f6bf47ec1c1993e",
    "kenji": "dfd2bc69bfc5e0071132c056860d452f1a5b9b41",
    "kenji-android": "280c427c550c5fe3bcc46b378e06a9b90a7e6b09b",
}
MENU = "src/Ryujinx/UI/Views/Main/MainMenuBarView.axaml"
ANDROID_SETTINGS = "src/KenjinxAndroid/app/src/main/java/org/kenjinx/android/views/SettingViews.kt"


def git(directory, *args):
    return subprocess.check_output(["git", "-C", str(directory), *args])


def replace_once(file, needle, inserted):
    text = file.read_text(encoding="utf-8")
    if text.count(needle) != 1:
        raise ValueError(f"Expected one occurrence of the upstream anchor in {file.name}")
    file.write_text(text.replace(needle, inserted, 1), encoding="utf-8")


def desktop(directory, target):
    prefix = "ext" if target == "ryubing" else "locale"
    menu_anchor = f'            <MenuItem VerticalAlignment="Center" Header="{{{prefix}:Locale MenuBarHelp}}">'
    replace_once(directory / MENU, menu_anchor,
                 '            <MenuItem Header="RoomCircuit" Click="OpenRoomCircuit" />\n' + menu_anchor)

    entry = '        public MainMenuBarView()'
    path_expr = "Program.GlobalConfigurationPath ?? Program.ConfigurationPath" if target == "ryubing" else "Program.ConfigurationPath"
    replace_once(directory / (MENU + ".cs"), entry, f'''
        private bool _roomCircuitOpen;

        private async void OpenRoomCircuit(object sender, Avalonia.Interactivity.RoutedEventArgs e)
        {{
            if (_roomCircuitOpen || Window is not MainWindow owner)
            {{
                return;
            }}

            _roomCircuitOpen = true;
            try
            {{
                string directory = Ryujinx.Common.Configuration.AppDataManager.BaseDirPath;
                var panel = new RoomCircuit.RoomCircuitWindow(
                    Path.Combine(directory, "RoomCircuit.json"), SaveRoomCircuitRelay,
                    System.Globalization.CultureInfo.CurrentUICulture.Name);
                await panel.ShowDialog(owner);
            }}
            finally
            {{
                _roomCircuitOpen = false;
            }}
        }}

        private void SaveRoomCircuitRelay(string host)
        {{
            if (ViewModel.IsGameRunning)
            {{
                throw new RoomCircuit.RoomCircuitException("game_running");
            }}

            var config = ConfigurationState.Instance;
            var multiplayer = config.Multiplayer;
            var previous = (multiplayer.Mode.Value, multiplayer.LdnServer.Value, multiplayer.DisableP2p.Value,
                multiplayer.LdnPassphrase.Value, config.System.EnableInternetAccess.Value);
            try
            {{
                multiplayer.Mode.Value = Ryujinx.Common.Configuration.Multiplayer.MultiplayerMode.LdnRyu;
                multiplayer.LdnServer.Value = host;
                multiplayer.DisableP2p.Value = true;
                multiplayer.LdnPassphrase.Value = string.Empty;
                config.System.EnableInternetAccess.Value = false;
                config.ToFileFormat().SaveConfig({path_expr});
            }}
            catch
            {{
                (multiplayer.Mode.Value, multiplayer.LdnServer.Value, multiplayer.DisableP2p.Value,
                    multiplayer.LdnPassphrase.Value, config.System.EnableInternetAccess.Value) = previous;
                throw;
            }}
        }}

''' + entry)
    source_dir = directory / "src/Ryujinx/UI/RoomCircuit"
    source_dir.mkdir(parents=True, exist_ok=True)
    for source in sorted((ROOT / "desktop").glob("*.cs")):
        (source_dir / source.name).write_bytes(source.read_bytes())


def android(directory):
    # The branch's upstream setting layout is stable at the supported revision.
    anchor = '                            // Language & Region'
    replace_once(directory / ANDROID_SETTINGS, anchor, '''
                            Button(onClick = {
                                RoomCircuitDialog.show(mainViewModel.activity, java.io.File(MainActivity.AppPath))
                            }) {
                                Text("RoomCircuit")
                            }

''' + anchor)
    for source in sorted((ROOT / "android").glob("*.kt")):
        (directory / ANDROID_SETTINGS).with_name(source.name).write_bytes(source.read_bytes())


def prepare(checkout, target, output, include_sockets):
    commit = PINS[target]
    relevant = [ANDROID_SETTINGS] if target == "kenji-android" else [MENU, MENU + ".cs"]
    transport = PROJECT / "server/ryuldn"
    dependencies = [transport / "kenji-android-ldn-server.patch"] if target == "kenji-android" else []
    if include_sockets:
        dependencies.append(transport / "ryu-client-sockets.patch")

    for patch in dependencies:
        for line in patch.read_text().splitlines():
            match = re.fullmatch(r"diff --git a/(\S+) b/(\S+)", line)
            if match and match.group(1) == match.group(2):
                relevant.append(match.group(1))

    with tempfile.TemporaryDirectory(prefix="rc-prepare-ryu-") as temp:
        directory = Path(temp)
        git(directory, "init", "-q")
        for relative in sorted(set(relevant)):
            file = directory / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(git(checkout, "show", f"{commit}:{relative}"))
        git(directory, "add", ".")
        git(directory, "-c", "user.name=RoomCircuit patch baseline",
            "-c", "user.email=patch-baseline@invalid.invalid", "commit", "-qm", "Pinned emulator source")
        for patch in dependencies:
            git(directory, "apply", str(patch))
        if target == "kenji-android":
            android(directory)
        else:
            desktop(directory, target)
        git(directory, "add", ".")
        patch = git(directory, "diff", "--cached", "--binary")
        git(directory, "reset", "-q", "--hard", "HEAD")
        candidate = directory / "candidate.patch"
        candidate.write_bytes(patch)
        git(directory, "apply", "--check", str(candidate))

        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(patch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=PINS)
    parser.add_argument("--checkout", required=True, type=Path, help="Git checkout containing the pinned revision")
    parser.add_argument("--output", required=True, type=Path, help="Path for the validated git patch")
    parser.add_argument("--include-sockets", action="store_true", help="Include the separately reviewable RyuLDN socket fixes")
    args = parser.parse_args()
    prepare(args.checkout.resolve(), args.target, args.output.resolve(), args.include_sockets)
