        $edit = [Windows.Automation.ControlType]::Edit
        $box = [Windows.Automation.ControlType]::CheckBox
        $button = [Windows.Automation.ControlType]::Button
        $combo = [Windows.Automation.ControlType]::ComboBox
        $filter = Require-Pattern (Find-Control 'consoleTextFilter' $edit) ([Windows.Automation.ValuePattern]::Pattern)
        $view = Find-Control 'consoleLogView' $edit
        $text = Require-Pattern $view ([Windows.Automation.TextPattern]::Pattern)
        $viewValue = Require-Pattern $view ([Windows.Automation.ValuePattern]::Pattern)
        Assert-That $viewValue.Current.IsReadOnly 'Log view is read-only'
        Assert-That (!$filter.Current.IsReadOnly) 'Text filter is writable'
        foreach ($id in @('consoleSessionFilter','consoleOperationFilter')) {
            $null = Require-Pattern (Find-Control $id $edit) ([Windows.Automation.ValuePattern]::Pattern)
        }
        foreach ($id in @('consoleSeverityFilter','consoleComponentFilter','consoleCopyScope')) {
            $null = Find-Control $id $combo
        }
        $null = Require-Pattern (Find-Control 'consoleCopy' $button) ([Windows.Automation.InvokePattern]::Pattern)
        $null = Find-Control 'consoleSaveLogs' $box $false
        $null = Find-Control 'consoleDiagnosticMode' $box $false
        $toggle = Require-Pattern (Find-Control 'consoleAutoScroll' $box) ([Windows.Automation.TogglePattern]::Pattern)
        $clear = Require-Pattern (Find-Control 'consoleClear' $button) ([Windows.Automation.InvokePattern]::Pattern)
        $null = Wait-For 'seeded log text exposed through TextPattern' {
            $s = $text.DocumentRange.GetText(-1)
            $s.Contains('session_invalidated') -and $s.Contains('native_cleanup_failed')
        }
        Assert-That ($toggle.Current.ToggleState -eq [Windows.Automation.ToggleState]::On) 'Auto-scroll initially On'
        $script:step = 'Toggle auto-scroll Off'
        $toggle.Toggle()
        $null = Wait-For 'Auto-scroll Off after Toggle' { $toggle.Current.ToggleState -eq [Windows.Automation.ToggleState]::Off }
        $toggle.Toggle()
        $null = Wait-For 'Auto-scroll On after second Toggle' { $toggle.Current.ToggleState -eq [Windows.Automation.ToggleState]::On }
        $script:step = 'Value.SetValue filter'
        $filter.SetValue('session_invalidated')
        $null = Wait-For 'Value and Text reflect matching filter' {
            $s = $text.DocumentRange.GetText(-1)
            $filter.Current.Value -eq 'session_invalidated' -and
                $s.Contains('session_invalidated') -and !$s.Contains('native_cleanup_failed')
        }
        $filter.SetValue('UIA_NO_MATCH_742')
        $null = Wait-For 'Nonmatching filter produces empty text' {
            $filter.Current.Value -eq 'UIA_NO_MATCH_742' -and [string]::IsNullOrWhiteSpace($text.DocumentRange.GetText(-1))
        }
        $filter.SetValue('')
        $null = Wait-For 'Clearing filter restores both cached entries' {
            $s = $text.DocumentRange.GetText(-1)
            $filter.Current.Value -eq '' -and $s.Contains('session_invalidated') -and $s.Contains('native_cleanup_failed')
        }
        $script:step = 'Invoke Clear'
        $clear.Invoke()
        $null = Wait-For 'Invoke Clear empties visible text' { [string]::IsNullOrWhiteSpace($text.DocumentRange.GetText(-1)) }
        $filter.SetValue('session_invalidated')
        $null = Wait-For 'Clear also empties cache after refilter' {
            $filter.Current.Value -eq 'session_invalidated' -and [string]::IsNullOrWhiteSpace($text.DocumentRange.GetText(-1))
        }
