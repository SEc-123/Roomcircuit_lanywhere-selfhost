#!/usr/bin/env python3
"""Prepare the patched MeloNX LDN provider for a Linux kernel socket test."""
import argparse
import hashlib
import importlib.util
from io import BytesIO
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('route', HERE.parent / 'melonx-verify.py')
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('checkout', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ldn = 'src/Ryujinx.HLE/HOS/Services/Ldn'
lib = ldn + '/UserServiceCreator/LdnMitm'
sock = 'src/Ryujinx.HLE/HOS/Services/Sockets/Bsd/Impl/ManagedSocket.cs'
files = subprocess.check_output(['git','-C',str(args.checkout),'ls-tree','-r','--name-only',route.PIN,lib,ldn+'/Types'],text=True).splitlines()
files += [ldn+'/LdnConst.cs', ldn+'/UserServiceCreator/Types/NetworkError.cs', sock,
          'src/Ryujinx.Common/Memory/IArray.cs','src/Ryujinx.Common/Memory/StructArrayHelpers.cs',
          'src/Ryujinx.Common/Utilities/NetworkHelpers.cs','src/Ryujinx.Common/Utilities/SpanHelpers.cs']
with tempfile.TemporaryDirectory() as tmp:
    project = Path(tmp)
    for file in files:
        target = project / file
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(subprocess.check_output(['git','-C',str(args.checkout),'show',route.PIN+':'+file]))
    subprocess.run(['git','init','-q'],cwd=project,check=True)
    subprocess.run(['git','apply','--include='+lib+'/*','--include='+sock,str(route.PATCH)],cwd=project,check=True)
    (project/lib/'LdnMitmClient.cs').unlink()
    (project/'FixtureSocket.cs').write_text(route.socket_fixture(project/sock))
    (project/sock).unlink()
    with urllib.request.urlopen('https://api.nuget.org/v3-flatcontainer/netcoreserver/8.0.7/netcoreserver.8.0.7.nupkg',timeout=20) as response:
        pkg=response.read()
    if hashlib.sha256(pkg).hexdigest()!='45462273cb8080919d8540ab3094142ca94707ac6fc3d1f59673465356b1359c':
        raise SystemExit("Unexpected NetCoreServer 8.0.7 archive")
    (project/'NetCoreServer.dll').write_bytes(zipfile.ZipFile(BytesIO(pkg)).read('lib/net8.0/NetCoreServer.dll'))
    shutil.copyfile(HERE/'Program.cs',project/'Program.cs')
    (project/'Melo.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net9.0</TargetFramework><AllowUnsafeBlocks>true</AllowUnsafeBlocks></PropertyGroup><ItemGroup><Reference Include="NetCoreServer" HintPath="NetCoreServer.dll"/></ItemGroup></Project>')
    subprocess.run(['docker','run','--rm','--network','none','-v',f'{project}:/src','-v',f'{out}:/out','-w','/src','mcr.microsoft.com/dotnet/sdk:9.0','dotnet','publish','-o','/out','-v','quiet'],check=True)
