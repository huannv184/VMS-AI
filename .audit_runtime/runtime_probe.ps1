param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("GET", "POST", "PUT", "DELETE", "PATCH")]
    [string]$Method,

    [Parameter(Mandatory = $true)]
    [string]$Url,

    [string]$Body = "",

    [string]$BearerToken = "",

    [int]$TimeoutMs = 5000
)

$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Net.Http

$handler = New-Object System.Net.Http.HttpClientHandler
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromMilliseconds($TimeoutMs)

$request = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::$Method, $Url)
if ($BearerToken) {
    $request.Headers.Authorization = New-Object System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", $BearerToken)
}
if ($Body) {
    $request.Content = New-Object System.Net.Http.StringContent($Body, [System.Text.Encoding]::UTF8, "application/json")
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$status = $null
$content = $null
$errorMessage = $null

try {
    $response = $client.SendAsync($request).GetAwaiter().GetResult()
    $status = [int]$response.StatusCode
    $content = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
} catch {
    $errorMessage = $_.Exception.Message
    if ($_.Exception.InnerException) {
        $errorMessage = "$errorMessage | INNER: $($_.Exception.InnerException.Message)"
    }
}

$sw.Stop()
$result = [ordered]@{
    status = $status
    elapsed_ms = [math]::Round($sw.Elapsed.TotalMilliseconds, 3)
    body = $content
    error = $errorMessage
}

$result | ConvertTo-Json -Compress -Depth 8
