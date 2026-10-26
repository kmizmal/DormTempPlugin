; ============================================================================
;  Chart.inc —— 一张历史曲线卡（可换区间、可换序列）
;  调用方在自己的 [Variables] 里给：
;    ChartKey     曲线标识，同一皮肤内唯一（同 key 只渲染一次）
;    ChartWindow  Today | 24h | 7d，留空则用 ChartHours
;    ChartHours   自定义小时数（1 ~ 2160）
;    ChartTitle   卡片标题
;    ChartSeries  Temp | Hum
;    ChartColor   曲线颜色 r,g,b
;  曲线由插件在自己的线程里画成 PNG，这里只是把图片贴上来
; ============================================================================
[Variables]
ChartKey=24h
ChartWindow=24h
ChartHours=24
ChartTitle=近 24 小时
ChartSeries=Temp
ChartColor=120,196,255
ChartUnit=°C

ChartW=224
ChartH=172
ChartPlotX=34
ChartPlotW=172
ChartPlotY=36
ChartPlotH=92

; ============ 出图 ============
[MeasureChartPath]
Measure=Plugin
Plugin=DormTemp
Type=Chart
Chart=#ChartKey#
Window=#ChartWindow#
Hours=#ChartHours#
Buckets=96
Width=#ChartPlotW#
Height=#ChartPlotH#
Series=#ChartSeries#
Color=#ChartColor#
LogPath=#DataPath#
DeviceName=#DeviceName#
MacAddress=#MacAddress#
UpdateDivider=5
DynamicVariables=1

; ============ 坐标轴与说明 ============
[MeasureChartHi]
Measure=Plugin
Plugin=DormTemp
Type=ChartHi
Chart=#ChartKey#
Series=#ChartSeries#
UpdateDivider=10
DynamicVariables=1

[MeasureChartMid]
Measure=Plugin
Plugin=DormTemp
Type=ChartMid
Chart=#ChartKey#
Series=#ChartSeries#
UpdateDivider=10
DynamicVariables=1

[MeasureChartLo]
Measure=Plugin
Plugin=DormTemp
Type=ChartLo
Chart=#ChartKey#
Series=#ChartSeries#
UpdateDivider=10
DynamicVariables=1

[MeasureChartMin]
Measure=Plugin
Plugin=DormTemp
Type=ChartMin
Chart=#ChartKey#
Series=#ChartSeries#
UpdateDivider=10
DynamicVariables=1

[MeasureChartMax]
Measure=Plugin
Plugin=DormTemp
Type=ChartMax
Chart=#ChartKey#
Series=#ChartSeries#
UpdateDivider=10
DynamicVariables=1

[MeasureChartSpan]
Measure=Plugin
Plugin=DormTemp
Type=ChartSpan
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

[MeasureChartSeriesName]
Measure=Plugin
Plugin=DormTemp
Type=ChartSeries
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

; ============ 外观 ============
[ChartBg]
Meter=Shape
Shape=Rectangle 0,0,#ChartW#,#ChartH#,12 | Fill Color #ColorCard# | StrokeWidth 1 | Stroke Color #ColorCardEdge#

[ChartTitle]
Meter=String
MeterStyle=StyleLabel
X=18
Y=12
Text=#ChartTitle#

[ChartSeries]
Meter=String
MeterStyle=StyleSmall
X=(#ChartW#-18)
Y=13
StringAlign=Right
MeasureName=MeasureChartSeriesName
Text=%1曲线

[ChartImage]
Meter=Image
X=#ChartPlotX#
Y=#ChartPlotY#
W=#ChartPlotW#
H=#ChartPlotH#
ImageName=[&MeasureChartPath]
PreserveAspectRatio=0
DynamicVariables=1

[ChartHi]
Meter=String
MeterStyle=StyleSmall
X=(#ChartPlotX#-6)
Y=(#ChartPlotY#-6)
StringAlign=Right
MeasureName=MeasureChartHi
Text=%1

[ChartMid]
Meter=String
MeterStyle=StyleSmall
X=(#ChartPlotX#-6)
Y=(#ChartPlotY#+(#ChartPlotH#/2)-6)
StringAlign=Right
MeasureName=MeasureChartMid
Text=%1

[ChartLo]
Meter=String
MeterStyle=StyleSmall
X=(#ChartPlotX#-6)
Y=(#ChartPlotY#+#ChartPlotH#-6)
StringAlign=Right
MeasureName=MeasureChartLo
Text=%1

[ChartFrom]
Meter=String
MeterStyle=StyleSmall
X=#ChartPlotX#
Y=(#ChartPlotY#+#ChartPlotH#+6)
MeasureName=MeasureChartSpan
Text=%1

[ChartTo]
Meter=String
MeterStyle=StyleSmall
X=(#ChartPlotX#+#ChartPlotW#)
Y=(#ChartPlotY#+#ChartPlotH#+6)
StringAlign=Right
Text=现在

[ChartFoot]
Meter=String
MeterStyle=StyleSmall
X=18
Y=(#ChartPlotY#+#ChartPlotH#+24)
MeasureName=MeasureChartMin
MeasureName2=MeasureChartMax
Text=低 %1　高 %2 #ChartUnit#
