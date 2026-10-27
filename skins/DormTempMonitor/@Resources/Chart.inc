; ============================================================================
;  Chart.inc —— 一张历史曲线卡（可换区间、可换序列）
;  调用方在自己的 [Variables] 里给：
;    ChartKey     曲线标识，同一皮肤内唯一（同 key 只渲染一次）
;    ChartWindow  Today | 24h | 7d，留空则用 ChartHours
;    ChartHours   自定义小时数（1 ~ 2160）
;    ChartTitle   卡片标题
;    ChartSeries  Both | Temp | Hum，Both（默认）温度走左轴、湿度走右轴画在同一张图上
;    ChartColor   温度线颜色 r,g,b
;    ChartColor2  湿度线颜色 r,g,b
;  曲线由插件在自己的线程里画成 PNG，这里只是把图片贴上来
; ============================================================================
[Variables]
ChartKey=24h
ChartWindow=24h
ChartHours=24
ChartTitle=近 24 小时
ChartSeries=Both
ChartColor=120,196,255
ChartColor2=127,224,176
ChartUnit=°C
ChartUnit2=%

; 坐标轴数字跟着曲线同色，一眼能对上左右两根轴
ChartAxisColor=120,196,255,170
ChartAxisColor2=127,224,176,170

; 卡片尺寸：左侧留给温度刻度，右侧留给湿度刻度
ChartW=248
ChartH=174
ChartPlotX=34
ChartPlotW=160
ChartPlotY=36
ChartPlotH=86

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
Color2=#ChartColor2#
LogPath=#DataPath#
DeviceName=#DeviceName#
MacAddress=#MacAddress#
UpdateDivider=5
DynamicVariables=1

; ============ 左轴（温度） ============
[MeasureChartHi]
Measure=Plugin
Plugin=DormTemp
Type=ChartHi
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

[MeasureChartMid]
Measure=Plugin
Plugin=DormTemp
Type=ChartMid
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

[MeasureChartLo]
Measure=Plugin
Plugin=DormTemp
Type=ChartLo
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

; ============ 右轴（湿度） ============
[MeasureChartHi2]
Measure=Plugin
Plugin=DormTemp
Type=ChartHi2
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

[MeasureChartMid2]
Measure=Plugin
Plugin=DormTemp
Type=ChartMid2
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

[MeasureChartLo2]
Measure=Plugin
Plugin=DormTemp
Type=ChartLo2
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

; ============ 极值 ============
[MeasureChartMin]
Measure=Plugin
Plugin=DormTemp
Type=ChartMin
Chart=#ChartKey#
Series=Temp
UpdateDivider=10
DynamicVariables=1

[MeasureChartMax]
Measure=Plugin
Plugin=DormTemp
Type=ChartMax
Chart=#ChartKey#
Series=Temp
UpdateDivider=10
DynamicVariables=1

[MeasureChartHumMin]
Measure=Plugin
Plugin=DormTemp
Type=ChartMin
Chart=#ChartKey#
Series=Hum
UpdateDivider=10
DynamicVariables=1

[MeasureChartHumMax]
Measure=Plugin
Plugin=DormTemp
Type=ChartMax
Chart=#ChartKey#
Series=Hum
UpdateDivider=10
DynamicVariables=1

[MeasureChartSpan]
Measure=Plugin
Plugin=DormTemp
Type=ChartSpan
Chart=#ChartKey#
UpdateDivider=10
DynamicVariables=1

; ============ 区间均值（悬停浮层用） ============
; 均值按窗口内每条样本加权，不是「桶均值的均值」，样本少的桶不会把结果带偏
[MeasureChartAvgTemp]
Measure=Plugin
Plugin=DormTemp
Type=ChartAvg
Chart=#ChartKey#
Series=Temp
UpdateDivider=10
DynamicVariables=1

[MeasureChartAvgHum]
Measure=Plugin
Plugin=DormTemp
Type=ChartAvg
Chart=#ChartKey#
Series=Hum
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

[ChartLegendTemp]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor#
X=(#ChartW#-18)
Y=13
StringAlign=Right
Text=温度 #ChartUnit#

[ChartLegendHum]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor2#
X=(#ChartW#-60)
Y=13
StringAlign=Right
Text=湿度 #ChartUnit2#

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
FontColor=#ChartAxisColor#
X=(#ChartPlotX#-6)
Y=(#ChartPlotY#-6)
StringAlign=Right
MeasureName=MeasureChartHi
Text=%1

[ChartMid]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor#
X=(#ChartPlotX#-6)
Y=(#ChartPlotY#+(#ChartPlotH#/2)-6)
StringAlign=Right
MeasureName=MeasureChartMid
Text=%1

[ChartLo]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor#
X=(#ChartPlotX#-6)
Y=(#ChartPlotY#+#ChartPlotH#-6)
StringAlign=Right
MeasureName=MeasureChartLo
Text=%1

[ChartHi2]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor2#
X=(#ChartPlotX#+#ChartPlotW#+6)
Y=(#ChartPlotY#-6)
StringAlign=Left
MeasureName=MeasureChartHi2
Text=%1

[ChartMid2]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor2#
X=(#ChartPlotX#+#ChartPlotW#+6)
Y=(#ChartPlotY#+(#ChartPlotH#/2)-6)
StringAlign=Left
MeasureName=MeasureChartMid2
Text=%1

[ChartLo2]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor2#
X=(#ChartPlotX#+#ChartPlotW#+6)
Y=(#ChartPlotY#+#ChartPlotH#-6)
StringAlign=Left
MeasureName=MeasureChartLo2
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

[ChartFootTemp]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor#
X=18
Y=(#ChartPlotY#+#ChartPlotH#+22)
MeasureName=MeasureChartMin
MeasureName2=MeasureChartMax
Text=温度 低 %1　高 %2 #ChartUnit#

[ChartFootHum]
Meter=String
MeterStyle=StyleSmall
FontColor=#ChartAxisColor2#
X=18
Y=(#ChartPlotY#+#ChartPlotH#+37)
MeasureName=MeasureChartHumMin
MeasureName2=MeasureChartHumMax
Text=湿度 低 %1　高 %2 #ChartUnit2#

; ============ 悬停浮层：本区间的温湿度均值 ============
; 三层都挂在 ChartHover 组里，靠最底下那个 ChartHover 命中区整组显隐
[ChartHoverBg]
Meter=Shape
Group=ChartHover
Hidden=1
Shape=Rectangle 0,0,156,26,8 | Fill Color 12,14,20,224 | StrokeWidth 1 | Stroke Color 255,255,255,26
X=(#ChartPlotX#+(#ChartPlotW#-156)/2)
Y=(#ChartPlotY#+(#ChartPlotH#-26)/2)

[ChartHoverTemp]
Meter=String
Group=ChartHover
Hidden=1
MeterStyle=StyleSmall
FontColor=#ChartAxisColor#
X=(#ChartPlotX#+(#ChartPlotW#-156)/2+10)
Y=(#ChartPlotY#+(#ChartPlotH#-26)/2+7)
MeasureName=MeasureChartAvgTemp
Text=均温 %1 #ChartUnit#

[ChartHoverHum]
Meter=String
Group=ChartHover
Hidden=1
MeterStyle=StyleSmall
FontColor=#ChartAxisColor2#
X=(#ChartPlotX#+(#ChartPlotW#-156)/2+146)
Y=(#ChartPlotY#+(#ChartPlotH#-26)/2+7)
StringAlign=Right
MeasureName=MeasureChartAvgHum
Text=均湿 %1 #ChartUnit2#

; 命中区：alpha 只有 1，肉眼看不见，但占位能接鼠标；放在最后 = 压在图上，
; 否则鼠标移到曲线/文字上就会触发 Leave，浮层闪个不停
[ChartHover]
Meter=Shape
Shape=Rectangle 0,0,#ChartW#,#ChartH#,12 | Fill Color 0,0,0,1 | StrokeWidth 0
MouseOverAction=[!SetOption ChartHoverBg Hidden 0][!SetOption ChartHoverTemp Hidden 0][!SetOption ChartHoverHum Hidden 0][!UpdateMeterGroup ChartHover][!Redraw]
MouseLeaveAction=[!SetOption ChartHoverBg Hidden 1][!SetOption ChartHoverTemp Hidden 1][!SetOption ChartHoverHum Hidden 1][!UpdateMeterGroup ChartHover][!Redraw]
DynamicVariables=1
