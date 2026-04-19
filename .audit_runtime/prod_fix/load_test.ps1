$uri = 'http://127.0.0.1:18320/api/health'
$total = [System.Diagnostics.Stopwatch]::StartNew()

$jobs = 1..40 | ForEach-Object {
    Start-Job -ScriptBlock {
        param($u)
        1..10 | ForEach-Object {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                Invoke-WebRequest -UseBasicParsing $u | Out-Null
            } catch {
            }
            $sw.Stop()
            [math]::Round($sw.Elapsed.TotalMilliseconds, 3)
        }
    } -ArgumentList $uri
}

$all = Receive-Job -Wait -AutoRemoveJob $jobs | ForEach-Object { [double]$_ }
$total.Stop()

$avg = [math]::Round((($all | Measure-Object -Average).Average), 3)
$sorted = $all | Sort-Object
$p95 = [math]::Round($sorted[[int][math]::Ceiling($sorted.Count * 0.95) - 1], 3)
$rps = [math]::Round($all.Count / $total.Elapsed.TotalSeconds, 2)

Write-Output ("count={0} rps={1} avg_ms={2} p95_ms={3} total_s={4}" -f `
    $all.Count,
    $rps,
    $avg,
    $p95,
    [math]::Round($total.Elapsed.TotalSeconds, 3))
