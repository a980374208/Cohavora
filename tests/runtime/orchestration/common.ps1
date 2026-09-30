function Assert-ProbeServiceUrl([string]$Value) {
    $uri=$null
    if(-not [Uri]::TryCreate($Value,[UriKind]::Absolute,[ref]$uri) -or
       $uri.Scheme -ne 'ws' -or -not $uri.Host -or $uri.Port -lt 1 -or
       $uri.UserInfo -or $uri.Query -or $uri.Fragment -or $uri.AbsolutePath -ne '/') {
        throw 'These controlled probes require a ws origin with explicit port, without credentials, query, or path'
    }
}
function Expand-ProbeCommand([string]$Command) {
    Assert-ProbeServiceUrl $ServiceUrl
    $uri=[Uri]$ServiceUrl
    $scheme=if($uri.Scheme -eq 'wss'){'https'}else{'http'}
    $port=if($uri.Port -gt 0){$uri.Port}elseif($uri.Scheme -eq 'wss'){443}else{80}
    $localUrl="${scheme}://127.0.0.1:$port"
    return $Command.Replace("'ws://123.56.225.164:17880'",($ServiceUrl | ConvertTo-Json -Compress)).Replace(
        "'http://127.0.0.1:17880'",($localUrl | ConvertTo-Json -Compress))
}
