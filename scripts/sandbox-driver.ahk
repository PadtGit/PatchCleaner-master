#Requires AutoHotkey v2.0
#SingleInstance Force

SetTitleMatchMode 2
DetectHiddenWindows True

if A_Args.Length < 9 {
    MsgBox "sandbox-driver.ahk requires 9 arguments."
    ExitApp 2
}

resultsRoot := A_Args[1]
sessionId := A_Args[2]
phase := A_Args[3]
installerDir := A_Args[4]
tempDir := A_Args[5]
moveTargetName := A_Args[6]
collisionTargetName := A_Args[7]
deleteTargetName := A_Args[8]
expectedInitialCount := Integer(A_Args[9])

resultPath := resultsRoot "\automation-" phase "-" sessionId ".txt"
logPath := resultsRoot "\automation-" phase "-" sessionId ".log"
state := Map()

Log(message) {
    global logPath
    timestamp := FormatTime(, "yyyy-MM-dd HH:mm:ss")
    FileAppend "[" timestamp "] " message "`n", logPath, "UTF-8"
}

SetState(name, value) {
    global state
    state[name] := value
}

WriteResults(status) {
    global state, resultPath, phase

    lines := ["phase=" phase, "status=" status]
    for key, value in state {
        lines.Push(key "=" value)
    }
    if FileExist(resultPath) {
        FileDelete resultPath
    }
    FileAppend StrJoin(lines, "`n") "`n", resultPath, "UTF-8"
}

StrJoin(items, separator) {
    output := ""
    for index, item in items {
        if index > 1 {
            output .= separator
        }
        output .= item
    }
    return output
}

WaitForWindow(winTitle, timeoutMs, excludeHwnd := 0) {
    deadline := A_TickCount + timeoutMs
    while A_TickCount < deadline {
        hwnd := WinExist(winTitle)
        if hwnd && hwnd != excludeHwnd {
            return hwnd
        }
        Sleep 250
    }
    return 0
}

ActivateWindow(hwnd, timeoutMs := 5000) {
    try {
        WinActivate "ahk_id " hwnd
        WinWaitActive "ahk_id " hwnd, , timeoutMs / 1000
        return true
    } catch {
        return false
    }
}

GetListViewHwnd(mainHwnd) {
    try {
        return ControlGetHwnd("SysListView321", "ahk_id " mainHwnd)
    } catch {
        return 0
    }
}

GetListCount(mainHwnd) {
    listHwnd := GetListViewHwnd(mainHwnd)
    if !listHwnd {
        return 0
    }
    count := SendMessage(0x1004, 0, 0, , "ahk_id " listHwnd)
    return Integer(count)
}

WaitForListCount(mainHwnd, minimum, timeoutMs) {
    deadline := A_TickCount + timeoutMs
    while A_TickCount < deadline {
        count := GetListCount(mainHwnd)
        if count >= minimum {
            return count
        }
        Sleep 500
    }
    return GetListCount(mainHwnd)
}

ClientClick(mainHwnd, x, y) {
    try {
        ControlClick Format("x{} y{}", x, y), "ahk_id " mainHwnd, , "Left", 1, "NA"
        return true
    } catch {
        return false
    }
}

ScaleForWindowDpi(mainHwnd, value) {
    dpi := DllCall("GetDpiForWindow", "ptr", mainHwnd, "uint")
    if !dpi {
        dpi := 96
    }
    return DllCall("MulDiv", "int", value, "int", dpi, "int", 96, "int")
}

GetActionButtonCenter(mainHwnd, buttonName, &x, &y) {
    try {
        WinGetClientPos ,, &clientWidth, &clientHeight, "ahk_id " mainHwnd
    } catch {
        return false
    }

    if (clientWidth <= 0 || clientHeight <= 0) {
        return false
    }

    outer := ScaleForWindowDpi(mainHwnd, 20)
    actionHeight := ScaleForWindowDpi(mainHwnd, 96)
    buttonHeight := ScaleForWindowDpi(mainHwnd, 42)
    buttonGap := ScaleForWindowDpi(mainHwnd, 10)
    buttonPaddingRight := ScaleForWindowDpi(mainHwnd, 24)
    deleteWidth := ScaleForWindowDpi(mainHwnd, 114)
    moveWidth := ScaleForWindowDpi(mainHwnd, 156)
    selectWidth := ScaleForWindowDpi(mainHwnd, 122)
    settingsWidth := ScaleForWindowDpi(mainHwnd, 112)
    buttonTop := clientHeight - outer - actionHeight + actionHeight - ScaleForWindowDpi(mainHwnd, 22) - buttonHeight
    settingsLeft := clientWidth - outer - buttonPaddingRight - settingsWidth
    deleteLeft := settingsLeft - buttonGap - deleteWidth
    moveLeft := deleteLeft - buttonGap - moveWidth
    selectLeft := moveLeft - buttonGap - selectWidth

    switch buttonName {
        case "select_all":
            left := selectLeft
            width := selectWidth
        case "delete":
            left := deleteLeft
            width := deleteWidth
        default:
            return false
    }

    x := left + Floor(width / 2)
    y := buttonTop + Floor(buttonHeight / 2)
    return true
}

SortList(mainHwnd) {
    pathClicked := ClientClick(mainHwnd, 70, 240)
    Sleep 500
    sizeClicked := ClientClick(mainHwnd, 900, 240)
    Sleep 500
    SetState("sort_interactions_sent", pathClicked && sizeClicked ? 1 : 0)
}

SelectAll(mainHwnd) {
    if !GetActionButtonCenter(mainHwnd, "select_all", &x, &y) {
        SetState("select_all_sent", 0)
        return false
    }

    clicked := ClientClick(mainHwnd, x, y)
    Sleep 500
    SetState("select_all_sent", clicked ? 1 : 0)
    return clicked
}

NudgeWindow(mainHwnd, step) {
    try {
        WinGetPos &x, &y, &width, &height, "ahk_id " mainHwnd
        deltaX := Mod(step, 2) = 0 ? 24 : -24
        deltaY := Mod(step, 2) = 0 ? 16 : -16
        targetX := x + deltaX
        targetY := y + deltaY
        if targetX < 0 {
            targetX := 0
        }
        if targetY < 0 {
            targetY := 0
        }
        flags := 0x4000 | 0x0010 | 0x0004 | 0x0001  ; ASYNCWINDOWPOS | NOACTIVATE | NOZORDER | NOSIZE
        return DllCall("SetWindowPos", "ptr", mainHwnd, "ptr", 0, "int", targetX,
            "int", targetY, "int", 0, "int", 0, "uint", flags, "int") != 0
    } catch {
        return false
    }
}

WaitForPathsAbsent(mainHwnd, paths, timeoutMs) {
    deadline := A_TickCount + timeoutMs
    moves := 0
    while A_TickCount < deadline {
        allGone := true
        for , path in paths {
            if FileExist(path) {
                allGone := false
                break
            }
        }
        if allGone {
            SetState("responsiveness_moves", moves)
            return true
        }
        if mainHwnd {
            if NudgeWindow(mainHwnd, moves) {
                moves += 1
            }
        }
        Sleep 750
    }
    SetState("responsiveness_moves", moves)
    return false
}

RunInitialPhase() {
    global expectedInitialCount, installerDir, tempDir, moveTargetName, collisionTargetName

    mainHwnd := WaitForWindow("Patch Cleaner ahk_class MainFrame", 30000)
    if !mainHwnd {
        SetState("note", "main_window_not_found")
        return false
    }

    SetState("initial_main_window_found", 1)
    ActivateWindow(mainHwnd)
    Sleep 500
    Send "{F5}"
    SetState("scan_sent", 1)
    Log "Sent F5 to start scan."

    promptHwnd := WaitForWindow("Patch Cleaner ahk_class #32770", 15000)
    if promptHwnd {
        SetState("relaunch_prompt_seen", 1)
        ActivateWindow(promptHwnd)
        Sleep 250
        Send "{Enter}"
        SetState("relaunch_prompt_accepted", 1)
        Log "Accepted the relaunch prompt."
    } else {
        SetState("relaunch_prompt_seen", 0)
        SetState("relaunch_prompt_accepted", 0)
    }

    Sleep 1500
    relaunchedHwnd := WaitForWindow("Patch Cleaner ahk_class MainFrame", 90000, mainHwnd)
    if !relaunchedHwnd {
        relaunchedHwnd := WaitForWindow("Patch Cleaner ahk_class MainFrame", 15000)
    }
    if !relaunchedHwnd {
        SetState("note", "elevated_window_not_found_after_scan")
        return false
    }

    SetState("relaunch_window_found", 1)
    mainHwnd := relaunchedHwnd
    ActivateWindow(mainHwnd)

    itemCount := WaitForListCount(mainHwnd, expectedInitialCount, 180000)
    SetState("post_scan_item_count", itemCount)
    if itemCount < expectedInitialCount {
        SetState("note", "initial_scan_item_count_below_expected")
        return false
    }

    SortList(mainHwnd)
    if !SelectAll(mainHwnd) {
        return false
    }

    PostMessage 0x111, 40001, 0, , "ahk_id " mainHwnd
    SetState("move_command_sent", 1)
    Log "Sent Move to Temp command."

    movePaths := [
        installerDir "\" moveTargetName,
        installerDir "\" collisionTargetName
    ]
    moveCompleted := WaitForPathsAbsent(mainHwnd, movePaths, 180000)
    SetState("move_completed", moveCompleted ? 1 : 0)

    collisionPattern := tempDir "\" RegExReplace(collisionTargetName, "\.msi$", " \[[^\]]+\]( \(\d+\))?\.msi")
    collisionFound := false
    loop files tempDir "\*.msi" {
        if RegExMatch(A_LoopFileName, "^PatchCleaner-Sandbox-Collision \[[^\]]+\]( \(\d+\))?\.msi$") {
            collisionFound := true
            break
        }
    }
    SetState("collision_tokenized_name_found", collisionFound ? 1 : 0)

    return moveCompleted && collisionFound
}

RunDeletePhase() {
    global installerDir, deleteTargetName

    mainHwnd := WaitForWindow("Patch Cleaner ahk_class MainFrame", 30000)
    if !mainHwnd {
        SetState("note", "main_window_not_found")
        return false
    }

    ActivateWindow(mainHwnd)
    Sleep 500
    Send "{F5}"
    SetState("delete_scan_sent", 1)
    Log "Sent F5 to refresh scan for delete phase."

    itemCount := WaitForListCount(mainHwnd, 1, 120000)
    SetState("delete_scan_item_count", itemCount)
    if itemCount < 1 {
        SetState("note", "delete_scan_item_count_zero")
        return false
    }

    if !SelectAll(mainHwnd) {
        return false
    }

    if !GetActionButtonCenter(mainHwnd, "delete", &x, &y) {
        SetState("delete_button_clicked", 0)
        SetState("note", "delete_button_coordinates_unavailable")
        return false
    }

    deleteClicked := ClientClick(mainHwnd, x, y)
    SetState("delete_button_clicked", deleteClicked ? 1 : 0)
    if !deleteClicked {
        SetState("note", "delete_button_click_failed")
        return false
    }

    deletePath := installerDir "\" deleteTargetName
    deleteCompleted := WaitForPathsAbsent(mainHwnd, [deletePath], 120000)
    SetState("delete_completed", deleteCompleted ? 1 : 0)
    return deleteCompleted
}

try {
    Log "Starting automation phase: " phase
    success := false
    if phase = "initial" {
        success := RunInitialPhase()
    } else if phase = "delete" {
        success := RunDeletePhase()
    } else {
        SetState("note", "unknown_phase")
        success := false
    }

    WriteResults(success ? "pass" : "revise")
    ExitApp(success ? 0 : 1)
} catch Error as err {
    SetState("note", RegExReplace(err.Message, "[\r\n]+", " "))
    WriteResults("revise")
    ExitApp 1
}
