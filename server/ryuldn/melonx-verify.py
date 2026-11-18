#!/usr/bin/env python3
"""Validate the MeloNX patch against pinned upstream sources and run its C# route helper."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

PIN = "51763377a60a662f9eed2df57745aa7deebdf8b9"
HERE = pathlib.Path(__file__).resolve().parent
PATCH = HERE / "melonx-wifi-gateway.patch"


def run(*argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def socket_fixture(source):
    """Compile the upstream Connect/SendTo/Disconnect methods verbatim; isolate unrelated BSD types."""
    code = source.read_text()

    def method(signature):
        start = code.index("        public " + signature)
        brace = code.index("{", start)
        depth = 0
        for index in range(brace, len(code)):
            depth += (code[index] == "{") - (code[index] == "}")
            if not depth:
                return code[start:index + 1]
        raise ValueError("Incomplete method " + signature)

    remote = next(line for line in code.splitlines() if "public IPEndPoint RemoteEndPoint" in line)
    return """
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnMitm;
using System;
using System.Net;
using System.Net.Sockets;
enum BsdSocketFlags { None }
enum LinuxError { SUCCESS, EINPROGRESS }
enum WsaError { WSAEWOULDBLOCK = 10035 }
static class WinSockHelper {
    public static LinuxError ConvertError(WsaError value) => throw new SocketException((int)value);
}
class FixtureSocket : IDisposable {
    public Socket Socket { get; } = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
    public SocketType SocketType => Socket.SocketType;
    public bool Blocking => Socket.Blocking;
    private IPEndPoint _guestBroadcastPeer;
    public void Dispose() => Socket.Dispose();
    private static SocketFlags ConvertBsdSocketFlags(BsdSocketFlags flags) => SocketFlags.None;
""" + "\n".join((remote, method("LinuxError Connect("), method("LinuxError SendTo("),
                     method("void Disconnect("), "}"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream", type=pathlib.Path, help="MeloNX Git checkout containing the pinned revision")
    parser.add_argument("--docker", action="store_true", help="Run the portable route test using the .NET 9 SDK Docker image")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rc-melonx-") as tmp:
        root = pathlib.Path(tmp)
        run("git", "init", "-q", str(root))
        for line in PATCH.read_text().splitlines():
            if line.startswith("--- a/"):
                name = line.removeprefix("--- a/")
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(run("git", "-C", str(args.upstream), "show", f"{PIN}:{name}", capture_output=True).stdout)
        run("git", "apply", "--check", str(PATCH), cwd=root)
        run("git", "apply", str(PATCH), cwd=root)
        print(f"MeloNX patch applies to {PIN}", flush=True)
        if shutil.which("swiftc"):
            swift = list(root.glob("src/**/*.swift"))
            run("swiftc", "-frontend", "-parse", *(str(p) for p in swift))
        test = root / "route-test"
        test.mkdir()
        shutil.copyfile(next(root.glob("src/**/RoomCircuitWifiRoute.cs")), test / "Route.cs")
        (test / "FixtureSocket.cs").write_text(socket_fixture(next(root.glob("src/**/ManagedSocket.cs"))))
        shutil.copyfile(HERE / "melonx-route-test.cs", test / "Program.cs")
        (test / "Check.csproj").write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net9.0</TargetFramework><TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup></Project>')
        if args.docker:
            namespace = run("docker", "run", "-d", "--network=none", "--cap-add", "NET_ADMIN",
                            "alpine:3.22", "sleep", "300", capture_output=True, text=True).stdout.strip()
            try:
                run("docker", "exec", namespace, "ip", "address", "add", "10.13.254.254/32", "dev", "lo")
                run("docker", "run", "--rm", "--network", "container:" + namespace, "-e", "DOTNET_NOLOGO=1",
                    "-v", f"{test}:/tests", "-w", "/tests", "mcr.microsoft.com/dotnet/sdk:9.0",
                    "dotnet", "run", "--project", "Check.csproj", "--", "wire")
            finally:
                run("docker", "rm", "-f", namespace, stdout=subprocess.DEVNULL)
        else:
            run("dotnet", "run", "--project", "Check.csproj", cwd=test)


if __name__ == "__main__":
    main()
