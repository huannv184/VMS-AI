param(
    [Parameter(Mandatory = $true)]
    [string]$Url,

    [int]$Requests = 400,
    [int]$Concurrency = 40,
    [string]$BearerToken = "",
    [int]$TimeoutMs = 5000,
    [int]$ProcessId = 0,
    [string]$LogPath = ""
)

$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http

function New-WorkerScript {
    param($Url, $Iterations, $BearerToken, $TimeoutMs)

    return {
        param($InnerUrl, $InnerIterations, $InnerBearerToken, $InnerTimeoutMs)

        Add-Type -AssemblyName System.Net.Http
        $handler = New-Object System.Net.Http.HttpClientHandler
        $client = New-Object System.Net.Http.HttpClient($handler)
        $client.Timeout = [TimeSpan]::FromMilliseconds($InnerTimeoutMs)
        if ($InnerBearerToken) {
            $client.DefaultRequestHeaders.Authorization = New-Object System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", $InnerBearerToken)
        }

        $latencies = New-Object System.Collections.Generic.List[Double]
        $failures = 0
        for ($i = 0; $i -lt $InnerIterations; $i++) {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                $response = $client.GetAsync($InnerUrl).GetAwaiter().GetResult()
                $null = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
                if (-not $response.IsSuccessStatusCode) {
                    $failures++
                }
            } catch {
                $failures++
            } finally {
                $sw.Stop()
                $latencies.Add($sw.Elapsed.TotalMilliseconds)
            }
        }

        [pscustomobject]@{
            latencies = $latencies
            failures = $failures
        }
    }.GetNewClosure()
}

$requestsPerWorker = [Math]::Floor($Requests / $Concurrency)
$remainder = $Requests % $Concurrency

$memBefore = $null
$memAfter = $null
if ($ProcessId -gt 0) {
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($proc) {
        $memBefore = $proc.WorkingSet64
    }
}

$logBefore = $null
if ($LogPath -and (Test-Path $LogPath)) {
    $logBefore = (Get-Item $LogPath).Length
}

$jobs = @()
$overall = [System.Diagnostics.Stopwatch]::StartNew()
for ($w = 0; $w -lt $Concurrency; $w++) {
    $iterations = $requestsPerWorker
    if ($w -lt $remainder) {
        $iterations++
    }
    $jobs += Start-Job -ScriptBlock (New-WorkerScript -Url $Url -Iterations $iterations -BearerToken $BearerToken -TimeoutMs $TimeoutMs) -ArgumentList $Url, $iterations, $BearerToken, $TimeoutMs
}

$results = Receive-Job -Job $jobs -Wait -AutoRemoveJob
$overall.Stop()

$allLatencies = New-Object System.Collections.Generic.List[Double]
$failures = 0
foreach ($result in $results) {
    $failures += $result.failures
    foreach ($lat in $result.latencies) {
        $allLatencies.Add([double]$lat)
    }
}

$sorted = $allLatencies | Sort-Object
$count = $sorted.Count
$avg = if ($count -gt 0) { ($sorted | Measure-Object -Average).Average } else { 0 }
$max = if ($count -gt 0) { $sorted[-1] } else { 0 }
$p95Index = if ($count -gt 0) { [Math]::Ceiling($count * 0.95) - 1 } else { 0 }
$p95 = if ($count -gt 0) { $sorted[$p95Index] } else { 0 }
$rps = if ($overall.Elapsed.TotalSeconds -gt 0) { $count / $overall.Elapsed.TotalSeconds } else { 0 }

if ($ProcessId -gt 0) {
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($proc) {
        $memAfter = $proc.WorkingSet64
    }
}

$logAfter = $null
if ($LogPath -and (Test-Path $LogPath)) {
    $logAfter = (Get-Item $LogPath).Length
}

[ordered]@{
    requests = $count
    concurrency = $Concurrency
    failures = $failures
    elapsed_s = [math]::Round($overall.Elapsed.TotalSeconds, 3)
    rps = [math]::Round($rps, 3)
    avg_ms = [math]::Round($avg, 3)
    p95_ms = [math]::Round($p95, 3)
    max_ms = [math]::Round($max, 3)
    mem_before = $memBefore
    mem_after = $memAfter
    mem_delta = if (($memBefore -ne $null) -and ($memAfter -ne $null)) { $memAfter - $memBefore } else { $null }
    log_before = $logBefore
    log_after = $logAfter
    log_delta = if (($logBefore -ne $null) -and ($logAfter -ne $null)) { $logAfter - $logBefore } else { $null }
} | ConvertTo-Json -Compress -Depth 6
