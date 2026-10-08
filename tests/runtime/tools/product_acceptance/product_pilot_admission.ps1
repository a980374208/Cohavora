# Bind each actual executable to its unique qualified role, path and bytes.
# The input manifest predates role metadata; these are the fixed target names
# emitted by the product/probe builds. An ambiguous manifest fails closed.
function Assert-ProductQualifiedTools($Gate,[string]$Executable,[string]$AudioCollector,[string]$GpuTraceTool) {
    $roles=@(
        @{name='product';leaf='Cohavora.exe';actual=$Executable},
        @{name='audio';leaf='product_audio_loopback.exe';actual=$AudioCollector},
        @{name='gpu';leaf='product_gpu_trace.exe';actual=$GpuTraceTool})
    foreach($role in $roles){
        $entries=@($Gate.inputs.PSObject.Properties | Where-Object {
            [IO.Path]::GetFileName($_.Name) -ieq $role.leaf
        })
        if($entries.Count -ne 1){throw ('FORMAL_TOOL_IDENTITY_MISSING_OR_AMBIGUOUS:'+ $role.name)}
        $entry=$entries[0]
        if($entry.Value -isnot [string] -or $entry.Value -cnotmatch '^[0-9a-f]{64}$'){
            throw ('FORMAL_TOOL_HASH_INVALID:'+ $role.name)
        }
        $actual=(Resolve-Path -LiteralPath $role.actual -ErrorAction Stop).ProviderPath
        $qualified=(Resolve-Path -LiteralPath $entry.Name -ErrorAction Stop).ProviderPath
        if(![string]::Equals($actual,$qualified,[StringComparison]::OrdinalIgnoreCase)){
            throw ('FORMAL_TOOL_PATH_CHANGED:'+ $role.name)
        }
        if((Get-FileHash -LiteralPath $actual -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.Value){
            throw ('FORMAL_TOOL_BYTES_CHANGED:'+ $role.name)
        }
    }
}
