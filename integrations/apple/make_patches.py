#!/usr/bin/env python3
"""Regenerate the two reviewable SwiftUI source patches from exact upstream Git objects."""

import argparse
import pathlib
import plistlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
MELO_REV = "13700519f3abc4e6d7ed8a065b118b2cc59bcd67"
POCKET_REV = "9c80cb793df2c2032e3f01153585a13150c7925d"
MELO = "src/MeloNX/MeloNX/"
POCKET_IOS = "dm/ios/pocket/"
POCKET_MAC = "dm/mac/pocket/"
POCKET_LAUNCHER = "dm/ios/Sources/LauncherHome.swift"
PANEL_FILES = ("RoomCircuitService.swift", "RoomCircuitPanel.swift")


def run(cwd, *command, stdin=None):
    return subprocess.run(command, cwd=cwd, check=True, input=stdin, stdout=subprocess.PIPE).stdout


def replace(text, needle, new):
    if text.count(needle) != 1:
        raise ValueError(f"Expected one upstream occurrence of {needle!r}")
    return text.replace(needle, new, 1)


def changes_melonx(files):
    settings = MELO + "UI/Main/Settings/SettingsView.swift"
    info = MELO + "Info.plist"
    text = files[settings]
    text = replace(text, "    @State private var showingKeyboardConfig = false\n",
                   "    @State private var showingKeyboardConfig = false\n"
                   "    @State private var showingRoomCircuit = false\n")
    text = replace(text, "        .onAppear { resolveConfiguredNetworkInterface() }\n",
                   "        .sheet(isPresented: $showingRoomCircuit) {\n"
                   "            RoomCircuitPanel(host: .meloNX, chinese: Locale.preferredLanguages.first?.hasPrefix(\"zh\") == true)\n"
                   "        }\n"
                   "        .onAppear { resolveConfiguredNetworkInterface() }\n")
    text = replace(text, "                Picker(\"Network Interface\", selection: selectedNetworkInterfaceId) {\n",
                   "                Button { showingRoomCircuit = true } label: {\n"
                   "                    Label(\"RoomCircuit\", systemImage: \"calendar\")\n"
                   "                }\n"
                   "                Picker(\"Network Interface\", selection: selectedNetworkInterfaceId) {\n")
    files[settings] = text
    files[info] = replace(files[info], "\t<key>CFBundleDocumentTypes</key>\n",
                          "\t<key>NSLocalNetworkUsageDescription</key>\n"
                          "\t<string>MeloNX connects to your RoomCircuit appointment server and local wireless devices.</string>\n"
                          "\t<key>NSAppTransportSecurity</key>\n"
                          "\t<dict>\n"
                          "\t\t<key>NSAllowsLocalNetworking</key>\n"
                          "\t\t<true/>\n"
                          "\t</dict>\n"
                          "\t<key>CFBundleDocumentTypes</key>\n")
    for name in PANEL_FILES:
        files[MELO + "UI/Main/Settings/" + name] = (HERE / "Sources" / name).read_text()


def changes_pocket(files):
    ios = POCKET_IOS + "PocketApp.swift"
    mac = POCKET_MAC + "App.swift"
    text = files[ios]
    start = "        w.rootViewController = LauncherHost(rootView: LauncherHome("
    if text.count(start) != 1:
        raise ValueError("Unexpected iOS launcher")
    text = replace(text, start, "        w.rootViewController = LauncherHost(rootView: PocketRoomCircuitHome(")
    files[ios] = text + """
// Pass the optional action into the shared launcher. The standalone game
// targets can keep using LauncherHome without compiling RoomCircuit.
private struct PocketRoomCircuitHome: View {
    @ObservedObject var library: GameLibrary
    let play: () -> Void
    let close: () -> Void
    @State private var showRoomCircuit = false

    var body: some View {
        LauncherHome(library: library, play: play, close: close, roomCircuit: { showRoomCircuit = true })
            .sheet(isPresented: $showRoomCircuit) {
                RoomCircuitPanel(host: .nxPocket, chinese: launcherChinese)
            }
    }
}
"""

    launcher = files[POCKET_LAUNCHER]
    launcher = replace(launcher, "    var m: LauncherModel { library.model }",
                       "    var roomCircuit: (() -> Void)? = nil\n    var m: LauncherModel { library.model }")
    launcher = replace(launcher, 'note: localPlayOn == nil ? tr("This game does not support local play.") : "")',
                       'note: localPlayOn == nil ? tr("This game does not support local play.") : "", roomCircuit: roomCircuit)')
    launcher = replace(launcher, "about: library.canImport)\n", "about: library.canImport, roomCircuit: roomCircuit)\n")
    launcher = replace(launcher, "    var about = false                       // NX Pocket's Settings: About NX Pocket, last\n",
                       "    var about = false                       // NX Pocket's Settings: About NX Pocket, last\n"
                       "    var roomCircuit: (() -> Void)? = nil\n")
    launcher = replace(launcher, "            if !sections.isEmpty || app || remove != nil {\n",
                       "            if !sections.isEmpty || app || remove != nil || roomCircuit != nil {\n")
    launcher = replace(launcher, "                    if let certificate { CertificateSection(changed: certificate) }\n",
                       "                    if let roomCircuit {\n"
                       "                        Section {\n"
                       "                            Button(action: roomCircuit) { Label(\"RoomCircuit\", systemImage: \"calendar\") }\n"
                       "                                .listRowBackground(Palette.panel)\n"
                       "                        }\n"
                       "                    }\n"
                       "                    if let certificate { CertificateSection(changed: certificate) }\n")
    files[POCKET_LAUNCHER] = launcher

    text = files[mac]
    text = replace(text, "    @StateObject private var model = AppModel()\n",
                   "    @StateObject private var model = AppModel()\n"
                   "    @Environment(\\.openWindow) private var openWindow\n")
    text = replace(text, "            CommandGroup(after: .toolbar) {\n",
                   "            CommandGroup(after: .toolbar) {\n"
                   "                Button(\"RoomCircuit…\") { openWindow(id: \"roomcircuit\") }\n")
    text = replace(text, "        Settings { SettingsView().environmentObject(model) }\n",
                   "        Window(\"RoomCircuit\", id: \"roomcircuit\") {\n"
                   "            RoomCircuitPanel(host: .nxPocket, chinese: launcherChinese, showsClose: false)\n"
                   "        }\n"
                   "        .defaultSize(width: 620, height: 640)\n"
                   "        Settings { SettingsView().environmentObject(model) }\n")
    files[mac] = text

    ios_project = POCKET_IOS + "project.yml"
    mac_project = POCKET_MAC + "project.yml"
    source_entries = "".join(f"      - path: {name}\n" for name in PANEL_FILES)
    files[ios_project] = replace(files[ios_project], "      - path: PocketApp.swift\n",
                                 "      - path: PocketApp.swift\n" + source_entries)
    files[ios_project] = replace(files[ios_project], "        NSLocalNetworkUsageDescription:",
                                 "        NSAppTransportSecurity:\n"
                                 "          NSAllowsLocalNetworking: true\n"
                                 "        NSLocalNetworkUsageDescription:")
    # No duplicate Swift files for macOS; both NX targets use exactly these sources.
    mac_entries = "".join(f"      - path: ../../ios/pocket/{name}\n" for name in PANEL_FILES)
    files[mac_project] = replace(files[mac_project], "      - path: App.swift\n",
                                 "      - path: App.swift\n" + mac_entries)
    files[mac_project] = replace(files[mac_project], "        NSHumanReadableCopyright:",
                                 "        NSLocalNetworkUsageDescription: \"NX Pocket connects to your RoomCircuit appointment server.\"\n"
                                 "        NSAppTransportSecurity:\n"
                                 "          NSAllowsLocalNetworking: true\n"
                                 "        NSHumanReadableCopyright:")
    for name in PANEL_FILES:
        files[POCKET_IOS + name] = (HERE / "Sources" / name).read_text()


def make(repo, revision, targets, transform, verify):
    files = {name: run(repo, "git", "show", f"{revision}:{name}").decode() for name in targets}
    with tempfile.TemporaryDirectory(prefix="roomcircuit-patch-") as folder:
        checkout = pathlib.Path(folder)
        run(checkout, "git", "init", "--quiet")

        def write_files():
            for name, data in files.items():
                output = checkout / name
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(data.encode())

        write_files()
        run(checkout, "git", "add", ".")
        run(checkout, "git", "-c", "user.name=RoomCircuit fixture", "-c", "user.email=fixture@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", revision)
        transform(files)
        write_files()
        run(checkout, "git", "add", ".")
        diff = run(checkout, "git", "-c", "color.ui=never", "diff", "--cached", "--no-ext-diff",
                   "--no-renames", "--binary", "--src-prefix=a/", "--dst-prefix=b/")
        if not diff:
            raise ValueError("No patch generated")
        # Check the exact bytes against the clean fixture index.
        run(checkout, "git", "apply", "--check", "--cached", "--reverse", "-", stdin=diff)
        run(checkout, "git", "restore", "--source=HEAD", "--staged", "--worktree", ".")
        run(checkout, "git", "apply", "--check", "-", stdin=diff)
        run(checkout, "git", "apply", "-", stdin=diff)
        if verify:
            swift_paths = [str(checkout / name) for name in files if name.endswith(".swift")]
            run(checkout, "swiftc", "-frontend", "-parse", "-enable-bare-slash-regex", *swift_paths)
            if transform is changes_melonx:
                info = plistlib.loads((checkout / (MELO + "Info.plist")).read_bytes())
                if not info["NSAppTransportSecurity"]["NSAllowsLocalNetworking"]:
                    raise ValueError("MeloNX local networking is not enabled")
        return diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--melonx", type=pathlib.Path)
    parser.add_argument("--nx-pocket", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not (args.melonx or args.nx_pocket):
        parser.error("Specify a pinned local MeloNX and/or NX Pocket Git repository")
    configurations = [
        (args.melonx, MELO_REV, "melonx-roomcircuit.patch",
         [MELO + "UI/Main/Settings/SettingsView.swift", MELO + "Info.plist"], changes_melonx),
        (args.nx_pocket, POCKET_REV, "nx-pocket-roomcircuit.patch",
         [POCKET_IOS + "PocketApp.swift", POCKET_IOS + "project.yml", POCKET_MAC + "App.swift",
          POCKET_MAC + "project.yml", POCKET_LAUNCHER], changes_pocket),
    ]
    for repo, revision, name, targets, transform in configurations:
        if repo is None:
            continue
        patch = HERE / name
        generated = make(repo.resolve(), revision, targets, transform, verify=args.check)
        if args.check:
            if patch.read_bytes() != generated:
                raise SystemExit(f"{name}: regeneration did not match")
        else:
            patch.write_bytes(generated)
        print(f"{name}: {len(generated.splitlines())} lines against {revision}")


if __name__ == "__main__":
    main()
