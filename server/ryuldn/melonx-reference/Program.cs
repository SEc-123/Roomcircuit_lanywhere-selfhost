// SPDX-License-Identifier: GPL-2.0-only
using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using Ryujinx.Common.Utilities;
using Ryujinx.HLE.HOS.Services.Ldn.Types;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnMitm;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.Types;

namespace Ryujinx.Common.Logging
{
    public enum LogClass { ServiceLdn }
    public class Sink { public void PrintMsg(LogClass cls,string text) => Console.Error.WriteLine(text); }
    public class Logger { public static Sink Error = new(), Warning = new(), Debug, Info; }
}

namespace Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnMitm
{
    class LdnMitmClient
    {
        public readonly ManualResetEventSlim Joined = new();
        public NetworkInfo Info;
        public void InvokeNetworkChange(NetworkInfo info,bool connected)
        {
            if (connected && info.Ldn.NodeCount==2) { Info=info; Joined.Set(); }
        }
    }
}

class Program
{
    static void Main(string[] args)
    {
        bool host = args[0]=="host", echo = args[2]=="echo";
        IPAddress local = IPAddress.Parse(args[1]);
        var harness=new LdnMitmClient();
        using var game=new FixtureSocket();
        game.Socket.Bind(new IPEndPoint(local,echo ? 5511:7788));
        game.Socket.ReceiveTimeout=12000;
        using var provider=new LanDiscovery(harness,local,IPAddress.Parse("255.255.255.252"),true);
        var user=new UserConfig(); Encoding.ASCII.GetBytes("Melo ref").CopyTo(user.UserName.AsSpan());
        var config=new NetworkConfig{NodeCountMax=4,LocalCommunicationVersion=42,
            IntentId=new IntentId{LocalCommunicationId=0x0100000000abc000}};
        if (host)
        {
            if (!provider.CreateNetwork(new SecurityConfig {SecurityMode=SecurityMode.Retail},user,config)) throw new Exception("CreateNetwork");
        }
        else
        {
            var filter=new ScanFilter{NetworkId=new NetworkId{IntentId=config.IntentId},Flag=ScanFilterFlag.LocalCommunicationId};
            var until=DateTime.UtcNow.AddSeconds(13);
            while(true)
            {
                var found=provider.Scan(0,filter);
                if (found.Length>0)
                {
                    if (found[0].Common.Ssid.Length > 0)
                    {
                        var named = filter;
                        named.Flag |= ScanFilterFlag.Ssid;
                        named.Ssid = found[0].Common.Ssid;
                        if (provider.Scan(0,named).Length==0) throw new Exception("SSID match missed");
                        named.Ssid.Name[0] ^= 1;
                        if (provider.Scan(0,named).Length!=0) throw new Exception("SSID contents ignored");
                    }
                    var status=provider.Connect(found[0],user,42);
                    if (status!=NetworkError.None) throw new Exception("Connect: "+status);
                    break;
                }
                if(DateTime.UtcNow>until)throw new Exception("Scan");
            }
        }
        if (!harness.Joined.Wait(15000))throw new Exception("Missing two-player network change");
        var remote=NetworkHelpers.ConvertUint(harness.Info.Ldn.Nodes[host?1:0].Ipv4Address);
        EndPoint from=new IPEndPoint(IPAddress.Any,0);
        var to=new IPEndPoint(remote,echo?7788:5511);
        var data=new byte[64];
        if (!echo) Send("nx-game");
        int n=game.Socket.ReceiveFrom(data,ref from);
        if(Encoding.ASCII.GetString(data,0,n)!=(echo?"nx-game":"peer-ok") || !((IPEndPoint)from).Address.Equals(remote))
            throw new Exception("Wrong source or game payload: "+from);
        if (echo) Send("peer-ok");
        Console.WriteLine("MeloNX "+args[0]+" admitted and exchanged game UDP");
        void Send(string message)
        {
            byte[] bytes=Encoding.ASCII.GetBytes(message);
            if (game.SendTo(out var sent,bytes,bytes.Length,BsdSocketFlags.None,to)!=LinuxError.SUCCESS || sent!=bytes.Length)
                throw new Exception("Game send");
        }
    }
}
