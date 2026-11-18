// SPDX-License-Identifier: MIT
package org.kenjinx.android.views

import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URI
import java.security.SecureRandom

internal class RoomCircuitApi(private val origin: URI, private val identity: String) {
    companion object {
        private val ipv4Part = Regex("^0$|^[1-9][0-9]{0,2}$")
        private val label = Regex("^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$")
        private val hex = "0123456789abcdef"
        val hexId = Regex("^[a-fA-F0-9]{16}$")
        val participant = Regex("^[a-fA-F0-9]{32}$")

        fun newIdentity(): String {
            val bytes = ByteArray(16)
            val random = SecureRandom()
            do random.nextBytes(bytes) while (bytes.all { it == 0.toByte() })
            return buildString(32) {
                for (byte in bytes) {
                    val value = byte.toInt() and 0xff
                    append(hex[value ushr 4]).append(hex[value and 0xf])
                }
            }
        }

        fun validHost(host: String): Boolean = host.length in 1..253 && host.split('.').all { label.matches(it) }

        fun parseOrigin(text: String): URI? {
            if (text.length > 512) return null
            val uri = runCatching { URI(text.trim()) }.getOrNull() ?: return null
            val host = uri.host ?: return null
            if (uri.userInfo != null || uri.rawQuery != null || uri.rawFragment != null ||
                (uri.rawPath != "" && uri.rawPath != "/") || (uri.port != -1 && uri.port !in 1..65535)) return null
            if (uri.scheme.equals("https", true)) return uri
            if (!uri.scheme.equals("http", true)) return null
            if (host == "localhost" || host == "[::1]") return uri
            val parts = host.split('.')
            if (parts.size != 4 || parts.any { !ipv4Part.matches(it) || it.toInt() > 255 }) return null
            val first = parts[0].toInt()
            val second = parts[1].toInt()
            return uri.takeIf { first == 10 || first == 127 || first == 172 && second in 16..31 || first == 192 && second == 168 }
        }
    }

    fun options(): JSONObject = send("/api/appointments/options", null)

    fun request(action: String, vararg fields: Pair<String, Any>): JSONObject {
        val body = JSONObject().put("action", action).put("identity", identity)
        fields.forEach { (name, value) -> body.put(name, value) }
        return send("/api/appointments/native", body)
    }

    private fun send(path: String, body: JSONObject?): JSONObject {
        val connection = origin.resolve(path).toURL().openConnection() as HttpURLConnection
        try {
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 8000
            connection.readTimeout = 8000
            connection.useCaches = false
            if (body != null) {
                connection.requestMethod = "POST"
                connection.setRequestProperty("Content-Type", "application/json")
                connection.setRequestProperty("X-RoomCircuit-Client", "native-v1")
                connection.doOutput = true
                val bytes = body.toString().toByteArray(Charsets.UTF_8)
                connection.setFixedLengthStreamingMode(bytes.size)
                connection.outputStream.use { it.write(bytes) }
            }
            val status = connection.responseCode
            val limit = 65536
            if (connection.contentLength > limit || connection.contentType?.substringBefore(';')?.trim() != "application/json") {
                throw IllegalStateException("invalid_response")
            }
            val input = if (status in 200..299) connection.inputStream else connection.errorStream
            val json = input?.use { stream ->
                val buffer = ByteArray(4096)
                val output = ByteArrayOutputStream()
                val deadline = System.nanoTime() + 8_000_000_000L
                while (true) {
                    if (System.nanoTime() > deadline) throw IllegalStateException("timeout")
                    val count = stream.read(buffer)
                    if (count < 0) break
                    if (output.size() + count > limit) throw IllegalStateException("invalid_response")
                    output.write(buffer, 0, count)
                }
                JSONObject(output.toString(Charsets.UTF_8.name()))
            } ?: throw IllegalStateException("invalid_response")
            if (status !in 200..299) throw IllegalStateException(json.optString("error", "unavailable"))
            return json
        } finally {
            connection.disconnect()
        }
    }
}
