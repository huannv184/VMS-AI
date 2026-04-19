const http = require('http');

async function testApi() {
    console.log("=== API TESTS ===");

    const request = (options, postData) => {
        return new Promise((resolve, reject) => {
            const req = http.request(options, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        resolve({ status: res.statusCode, body: JSON.parse(data || '{}') });
                    } catch (e) {
                        resolve({ status: res.statusCode, text: data });
                    }
                });
            });
            req.on('error', reject);
            if (postData) {
                req.write(postData);
            }
            req.end();
        });
    };

    try {
        // 1. INVALID REQUEST
        console.log("\n[TEST] Invalid Request");
        const resBad = await request({
            hostname: '127.0.0.1',
            port: 8100,
            path: '/api/invalid_endpoint',
            method: 'GET'
        });
        console.log(`Status: ${resBad.status}`);
        console.log(resBad.body);

        // 2. LOGIN (Without credentials)
        console.log("\n[TEST] Login (No Credentials)");
        const resLoginFail = await request({
            hostname: '127.0.0.1',
            port: 8100,
            path: '/api/auth/login',
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        }, JSON.stringify({}));
        console.log(`Status: ${resLoginFail.status}`);
        console.log(resLoginFail.body);

        // 3. LOGIN (Valid credentials)
        console.log("\n[TEST] Login (Valid Credentials)");
        const resLogin = await request({
            hostname: '127.0.0.1',
            port: 8100,
            path: '/api/auth/login',
            method: 'POST',
            headers: { 'Content-Type': 'application/json' }
        }, JSON.stringify({ username: "admin", password: "admin" }));
        console.log(`Status: ${resLogin.status}`);
        console.log(resLogin.body);

        // 4. EVENTS
        console.log("\n[TEST] Events List");
        const token = resLogin.body.data?.token || '';
        const headers = token ? { 'Authorization': `Bearer ${token}` } : {};
        const resEvents = await request({
            hostname: '127.0.0.1',
            port: 8100,
            path: '/api/events?event_type=ppe',
            method: 'GET',
            headers
        });
        console.log(`Status: ${resEvents.status}`);
        console.log(JSON.stringify(resEvents.body).substring(0, 200) + '...');
        
    } catch (e) {
        console.error("Test failed:", e);
    }
}

testApi();
