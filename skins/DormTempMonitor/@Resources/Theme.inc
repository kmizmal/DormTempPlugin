; ============================================================================
;  Theme.inc —— 所有 DormTemp 皮肤共用的变量与样式
;  统一换配色 / 字体只改这一个文件；单个皮肤里写同名变量可以覆盖
; ============================================================================
[Variables]
; 数据目录（SQLite dormtemp.db、曲线 PNG、插件日志都在这儿）
DataPath=%LOCALAPPDATA%\DormTempPlugin
; 传感器广播名
DeviceName=MJWSD06MMC
; 可选：固定 MAC（留空 = 自动扫描并记忆上次连上的设备）
MacAddress=

FontName=Microsoft YaHei UI
ColorMain=238,240,245,255
ColorDim=150,155,165,255
ColorFaint=118,124,136,255
ColorAccent=120,196,255,255
ColorWarn=255,176,92,255
ColorBg=16,18,24,205
ColorCard=255,255,255,8
ColorCardEdge=255,255,255,16

; ============ 样式 ============
[StyleLabel]
FontFace=#FontName#
FontSize=9
FontColor=#ColorDim#
AntiAlias=1

[StyleSmall]
FontFace=#FontName#
FontSize=7.5
FontColor=#ColorFaint#
AntiAlias=1

[StyleValue]
FontFace=#FontName#
FontSize=22
FontWeight=600
FontColor=#ColorAccent#
AntiAlias=1

[StyleUnit]
FontFace=#FontName#
FontSize=8
FontColor=#ColorDim#
AntiAlias=1
