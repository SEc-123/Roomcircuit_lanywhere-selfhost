#!/usr/bin/env python3
"""Read-only native TLS compatibility endpoint; no account or game authority."""
import http.server, json, os, socketserver, ssl

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def setup(self):
        super().setup()
        self.connection.settimeout(5)
    def do_GET(self):
        valid = self.path in ("/healthz", "/native/compatibility")
        body = json.dumps({"service": "roomcircuit-native-compatibility", "protocol": 1, "status": "ok"} if valid else {"error": "not_found"}, separators=(",", ":")).encode()
        self.send_response(200 if valid else 404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True
    def log_message(self, fmt, *args):
        pass

class Server(http.server.HTTPServer):
    request_queue_size = 16
    def get_request(self):
        sock, addr = super().get_request()
        sock.settimeout(5)
        try:
            return context.wrap_socket(sock, server_side=True), addr
        except Exception:
            sock.close()
            raise

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.minimum_version = ssl.TLSVersion.TLSv1_2
context.load_cert_chain(os.environ["RC_TLS_CERT"], os.environ["RC_TLS_KEY"])
server = Server(("0.0.0.0", 443), Handler)
server.serve_forever(poll_interval=0.5)
