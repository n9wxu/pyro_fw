/*
 * Fetch shim: intercepts /api/* calls in the iframe and routes
 * them to parent.simApi() which reads from the WASM module.
 * The real app.js runs unmodified — it thinks it's talking to a device.
 */
(function() {
    var realFetch = window.fetch;

    window.fetch = function(url, opts) {
        var path = (typeof url === 'string') ? url : url.url;
        if (path.indexOf('/api/') === -1 && path.indexOf('api/') !== 0)
            return realFetch.apply(this, arguments);

        var method = (opts && opts.method) || 'GET';
        var body = (opts && opts.body) || null;
        if (body && typeof body !== 'string') body = new TextDecoder().decode(body);

        var apiPath = path.replace(/.*\/api\//, '/api/');

        return new Promise(function(resolve) {
            var result = window.parent.simApi(apiPath, method, body);
            resolve(new Response(result.body, {
                status: result.status || 200,
                headers: { 'Content-Type': result.contentType || 'application/json' }
            }));
        });
    };

    /* Also intercept window.location assignment for CSV download */
    var origLocation = Object.getOwnPropertyDescriptor(window, 'location');
})();
