function Get-Median([double[]] $Values) {
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    $middle = [int][Math]::Floor($sorted.Length / 2)
    if ($sorted.Length % 2) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}

function Get-PairedEstimate([double[]] $Changes) {
    $median = Get-Median $Changes
    $random = [Random]::new(1701)
    $medians = [double[]]::new(10000)
    $resample = [double[]]::new($Changes.Length)
    for ($i = 0; $i -lt $medians.Length; ++$i) {
        for ($j = 0; $j -lt $resample.Length; ++$j) { $resample[$j] = $Changes[$random.Next($Changes.Length)] }
        $medians[$i] = Get-Median $resample
    }
    [Array]::Sort($medians)
    [ordered]@{
        median_pct = [Math]::Round($median, 3)
        bootstrap_95_pct = @([Math]::Round($medians[249], 3), [Math]::Round($medians[9749], 3))
    }
}
