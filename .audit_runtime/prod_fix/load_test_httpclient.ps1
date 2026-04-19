$uri = 'http://127.0.0.1:18320/api/health'
$totalRequests = 400
$maxConcurrency = 40

Add-Type -AssemblyName System.Net.Http

$handler = New-Object System.Net.Http.HttpClientHandler
$handler.UseProxy = $false
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromSeconds(5)

$latencies = [System.Collections.Concurrent.ConcurrentBag[double]]::new()
$errors = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$gate = [System.Threading.SemaphoreSlim]::new($maxConcurrency, $maxConcurrency)
$allTasks = [System.Collections.Generic.List[System.Threading.Tasks.Task]]::new()
$clock = [System.Diagnostics.Stopwatch]::StartNew()

for ($i = 0; $i -lt $totalRequests; $i++) {
    $gate.Wait()
    $task = [System.Threading.Tasks.Task]::Run([Action]{
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $response = $client.GetAsync($uri).GetAwaiter().GetResult()
            $null = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            if (-not $response.IsSuccessStatusCode) {
                $errors.Add("status=$([int]$response.StatusCode)")
            }
        } catch {
            $errors.Add($_.Exception.Message)
        } finally {
            $sw.Stop()
            $latencies.Add([math]::Round($sw.Elapsed.TotalMilliseconds, 3))
            $gate.Release() | Out-Null
        }
    })
    $allTasks.Add($task)
}

[System.Threading.Tasks.Task]::WaitAll($allTasks.ToArray())
$clock.Stop()
$client.Dispose()
$handler.Dispose()
$gate.Dispose()

$samples = $latencies.ToArray() | Sort-Object
$avg = [math]::Round((($samples | Measure-Object -Average).Average), 3)
$p95Index = [Math]::Max(0, [int][math]::Ceiling($samples.Count * 0.95) - 1)
$p95 = [math]::Round($samples[$p95Index], 3)
$rps = [math]::Round($samples.Count / $clock.Elapsed.TotalSeconds, 2)

Write-Output ("count={0} rps={1} avg_ms={2} p95_ms={3} total_s={4} errors={5}" -f `
    $samples.Count,
    $rps,
    $avg,
    $p95,
    [math]::Round($clock.Elapsed.TotalSeconds, 3),
    $errors.Count)

if ($errors.Count -gt 0) {
    $errors.ToArray() | Select-Object -First 10 | ForEach-Object {
        Write-Output ("error={0}" -f $_)
    }
    exit 1
}
