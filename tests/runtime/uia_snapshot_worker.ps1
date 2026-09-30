param([Parameter(Mandatory=$true)][string]$RequestFile,
      [Parameter(Mandatory=$true)][string]$ResultFile)
# Exactly one read-only request per process. No sleeps, event subscriptions,
# discovery fallback, Pattern actions, or product lifecycle operations.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$request=Get-Content -LiteralPath $RequestFile -Raw | ConvertFrom-Json
$product=Get-Process -Id $request.product_pid
try {
    if ($product.Path -ne $request.executable -or
        $product.StartTime.ToUniversalTime().Ticks -ne [long]$request.start_ticks) {throw 'PRODUCT_IDENTITY_CHANGED'}
    $ids=@($request.automation_ids)
    if (!$ids.Count -or $ids.Count -gt 16 -or @($ids|Select-Object -Unique).Count -ne $ids.Count) {throw 'INVALID_IDS'}
    $pidCondition=[Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$product.Id)
    $cache=[Windows.Automation.CacheRequest]::new()
    $cache.AutomationElementMode=[Windows.Automation.AutomationElementMode]::None
    $cache.TreeScope=[Windows.Automation.TreeScope]::Element
    foreach ($property in @('AutomationId','Name','ProcessId','ControlType','IsEnabled','IsOffscreen','NativeWindowHandle')) {
        $cache.Add([Windows.Automation.AutomationElement]::("${property}Property"))
    }
    $desktop=[Windows.Automation.AutomationElement]::RootElement
    $scope=$cache.Activate()
    try {$roots=$desktop.FindAll([Windows.Automation.TreeScope]::Children,$pidCondition)} finally {$scope.Dispose()}
    $values=@(foreach ($id in $ids) {
        if ([string]::IsNullOrWhiteSpace($id) -or $id.Length -gt 512) {throw 'INVALID_ID'}
        $condition=[Windows.Automation.AndCondition]::new($pidCondition,
            [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty,[string]$id))
        $matches=@(foreach ($root in $roots) {
            $info=$root.Cached
            if ($info.AutomationId -eq $id) {$info}
            if (!$info.NativeWindowHandle) {throw 'ROOT_HANDLE_MISSING'}
            $live=[Windows.Automation.AutomationElement]::FromHandle([IntPtr]$info.NativeWindowHandle)
            if ($live.Current.ProcessId -ne $product.Id) {throw 'ROOT_IDENTITY_CHANGED'}
            $scope=$cache.Activate()
            try {$nodes=$live.FindAll([Windows.Automation.TreeScope]::Descendants,$condition)} finally {$scope.Dispose()}
            foreach ($node in $nodes) {$node.Cached}
        })
        if ($matches.Count -ne 1) {throw 'CONTROL_COUNT'}
        $m=$matches[0]
        if ($m.ProcessId -ne $product.Id) {throw 'CONTROL_IDENTITY_CHANGED'}
        if ($m.Name.Length -gt 4096) {throw 'VALUE_TOO_LARGE'}
        @{automation_id=$m.AutomationId;name=$m.Name;role=$m.ControlType.ProgrammaticName;
          enabled=$m.IsEnabled;offscreen=$m.IsOffscreen}
    })
    $product.Refresh()
    if ($product.HasExited) {throw 'PRODUCT_EXITED'}
    @{schema=1;request_id=$request.request_id;product_pid=$product.Id;client_pid=$PID;
      utc=[DateTime]::UtcNow.ToString('o');values=$values;scope='read_only_exact_ids'} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ResultFile -Encoding UTF8
} finally {$product.Dispose()}
