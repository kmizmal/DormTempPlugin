; ============================================================================
;  Card.inc —— 一张汇总卡片（今日 / 24 小时 / 7 天共用这一份）
;  调用方只要在自己的 [Variables] 里给两个值：
;    CardWindow = Today | 24h | 7d
;    CardTitle  = 卡片标题
;  要别的区间（比如近 3 天）请用曲线卡：那边支持 Hours= 任意小时数
; ============================================================================
[Variables]
CardWindow=Today
CardTitle=今日均温
CardW=224
CardH=76

; ============ 数据 ============
[MeasureCardSummary]
Measure=Plugin
Plugin=DormTemp
Type=Summary
Window=#CardWindow#
LogPath=#DataPath#
DeviceName=#DeviceName#
MacAddress=#MacAddress#
UpdateDivider=30

[MeasureCardAvg]
Measure=Plugin
Plugin=DormTemp
Type=TempAvg
Window=#CardWindow#
UpdateDivider=30

[MeasureCardMax]
Measure=Plugin
Plugin=DormTemp
Type=TempMax
Window=#CardWindow#
UpdateDivider=30

[MeasureCardMin]
Measure=Plugin
Plugin=DormTemp
Type=TempMin
Window=#CardWindow#
UpdateDivider=30

[MeasureCardHum]
Measure=Plugin
Plugin=DormTemp
Type=HumAvg
Window=#CardWindow#
UpdateDivider=30

[MeasureCardCount]
Measure=Plugin
Plugin=DormTemp
Type=Samples
Window=#CardWindow#
UpdateDivider=30

; ============ 外观 ============
[CardBg]
Meter=Shape
Shape=Rectangle 0,0,#CardW#,#CardH#,12 | Fill Color #ColorCard# | StrokeWidth 1 | Stroke Color #ColorCardEdge#
ToolTipTitle=#CardTitle#
ToolTipText=[MeasureCardSummary]
DynamicVariables=1

[CardLabel]
Meter=String
MeterStyle=StyleLabel
X=18
Y=11
Text=#CardTitle#

[CardCount]
Meter=String
MeterStyle=StyleSmall
X=(#CardW#-18)
Y=12
StringAlign=Right
MeasureName=MeasureCardCount
Text=%1 条

[CardValue]
Meter=String
MeterStyle=StyleValue
X=16
Y=20
MeasureName=MeasureCardAvg
Text=%1

[CardUnit]
Meter=String
MeterStyle=StyleUnit
X=4R
Y=([CardValue:Y]+[CardValue:H]-4)
StringAlign=LeftBottom
DynamicVariables=1
Text=°C

[CardDetail]
Meter=String
MeterStyle=StyleSmall
X=18
Y=(#CardH#-20)
MeasureName=MeasureCardMax
MeasureName2=MeasureCardMin
MeasureName3=MeasureCardHum
Text=高 %1　低 %2　湿 %3%
