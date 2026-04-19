const net = require('net');
const crypto = require('crypto');

const key = crypto.randomBytes(16).toString('base64');
console.log("Starting WS connection test to ws://127.0.0.1:8083/ws...");

const client = net.createConnection({ port: 8183, host: '127.0.0.1' }, () => {
    console.log('TCP Connected! Sending WS Handshake...');
    const req = 
        "GET /ws HTTP/1.1\r\n" +
        "Host: 127.0.0.1:8083\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        "Sec-WebSocket-Key: " + key + "\r\n" +
        "Sec-WebSocket-Version: 13\r\n\r\n";
    client.write(req);

    // Wait a brief moment before ending to see if server responds
    setTimeout(() => {
        client.end();
    }, 2000);
});

client.on('data', (data) => {
    console.log("RECEIVED:");
    console.log(data.toString());
});
client.on('end', () => {
    console.log("Disconnected.");
});
client.on('error', (err) => {
    console.error("WS error:", err);
});
