# 联想笔记本电脑风扇控制

Language: [English](README.md)

---

在Windows系统使用`Lenovo ACPI-Compliant Virtual Power Controller`驱动控制联想笔记本电脑风扇。

本项目是专为联想笔记本电脑准备的，尤其是那些风扇无法用主流风扇控制工具（如Notebook FanControl, SpeedFan, Fan Control）控制的电脑。

但是，本项目并不是一个风扇控制的完美解决方案。本项目所使用的方法仅能实现控制风扇以最低、最高和正常转速运行，而无法控制风扇以其他特定的转速运行。

本程序仅提供控制风扇这一简单功能，如果要实现温度监控、风扇曲线控制等等更复杂的功能，可以使用[FanControl](https://github.com/Rem0o/FanControl.Releases)程序，再安装上基于本项目开发的[FanControl.LenovoPlugin](https://github.com/jiarandiana0307/FanControl.LenovoPlugin)风扇控制插件，即可实现更强大的风扇控制功能。

# 关于本分支

本项目是 jiarandiana0307 的 [Lenovo-Fan-Control](https://github.com/jiarandiana0307/Lenovo-Fan-Control) 的修改版，使用相同的 MIT 许可证。主要改动：

- 风扇默认以**正常转速**启动（原版默认以高转速启动）。添加`--high-speed`参数可恢复原版行为。
- 新增`开机自启动`菜单项，可让程序在登录 Windows 时自动运行（以正常转速）。
- 无法识别的命令行参数现在会被忽略，不再把启动转速重置为高转速。

# 使用前提

- 联想笔记本电脑
- Windows操作系统
- 已安装联想驱动：`Lenovo ACPI-Compliant Virtual Power Controller`

# 使用方法

1. 从本项目的[发布页面](../../releases)下载编译好的程序

2. 双击LenovoFanControl程序运行，然后你就能在系统托盘看到这个程序。

如果有弹窗提示`无法访问\\.\EnergyDrv`，这说明没找到联想驱动或驱动异常。如果没有弹窗，说明程序正常运行，此时风扇会以正常转速运转。

![程序菜单截图](github/menu-screenshot-zh_CN.jpg)

点击系统托盘中的程序图标会弹出一个菜单，程序菜单首行显示当前风扇的运行状态，可能是以下三种状态：

1. `低转速`：此时风扇正以最低转速运转。
2. `高转速`：此时风扇正以最高转速运转。
3. `正常转速`：此时风扇正以正常转速运转。

此时可以点击程序菜单的`低转速`和`高转速`，或者按下相应的快捷键`Ctrl+Alt+F10`和`Ctrl+Alt+F11`，让风扇在最低转速和最高转速之间切换。另外，点击程序菜单的`正常转速`或快捷键`Ctrl+Alt+F12`可以让风扇恢复正常转速。

最后，点击程序菜单中的`退出`即可终止程序，随后风扇会恢复为正常转速。

勾选程序菜单中的`开机自启动`，程序就会在登录 Windows 时自动运行；取消勾选即可关闭。该选项会在`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`下添加一项，以`--normal-speed`参数启动程序，无需管理员权限。如果移动了程序文件的位置，请取消勾选后重新勾选以更新路径。

在运行程序时添加`--low-speed`、`--normal-speed`和`--high-speed`参数，能够手动选择风扇在开始时分别以低转速、正常转速和高转速运行。如果不添加这些参数，则风扇默认以正常转速运行。例如，如果你想在程序开始运行时让风扇保持低转速，可以运行命令：`LenovoFanControl-x64.exe --low-speed`

**注意：**请谨慎使用`低转速`模式，因为本程序没有温度监控功能，所以在`低转速`模式下容易引起硬件高温从而导致硬件损坏。

# 实现原理

在一般情况下，笔记本电脑的风扇都是由嵌入式控制器（简称EC）控制的，它主要负责管理低速外设，如触摸板、风扇等。因此如果EC给风扇提供不同的电压，风扇就能以不同转速运行。我们可以通过修改EC寄存器的值来实现我们想要的功能，但对于某些机型，数据手册中并没有说明控制风扇的是哪个寄存器，这也就无法直接通过EC来控制风扇了。然后，有人对联想电源管理器程序进行了逆向，来研究它的风扇除尘功能是如何实现的。最后发现这个功能是通过`Lenovo ACPI-Compliant Virtual Power Controller`驱动来控制EC，进而控制风扇的。其实这也是本项目的实现原理。

如果你安装了`Lenovo ACPI-Compliant Virtual Power Controller`驱动，那么你的系统中会有一个名为`\\.\EnergyDrv`的设备，此设备正是由这个联想驱动生成的，驱动通过这个设备给其他用户程序提供交互的接口，其他程序可以通过读写这个设备来实现对驱动的控制。这个驱动提供了一个能够控制风扇的除尘功能，我们通过调用win32 API可以轻易地对这个设备的特定字节进行读写，从而控制驱动，让驱动控制EC，最后控制风扇。以下是示意图。

![示意图](github/diagram-zh_CN.jpg)

但这样控制风扇有一个问题，就是风扇的运行是断断续续的。当控制驱动执行除尘操作时，风扇会以最高转速运行9秒，然后暂停2秒，然后又运行9秒暂停2秒，循环往复直到2分钟后恢复正常状态。除尘功能是由EC自动控制的，有时它甚至会在风扇高速旋转的9秒中的任意时刻突然停下。

为了解决这个问题，首先，我们先让驱动执行除尘操作，等待9秒，然后手动让驱动终止除尘操作以此来重置9秒的运行时间，随后在风扇停转前立即让驱动重新执行除尘操作，再等待9秒，如此循环执行。通过这样在风扇启停间快速的切换，除了在每次切换时风扇速度会短暂小幅下降，风扇能够一直以最高转速运行不会停止。

# 免责声明

本项目项目不对任何可能的设备损坏负责，使用风险由使用者自行承担。

# 参考资料

- [IdeaFan][IdeaFan]
- [bitrate16/FanControl][FanControl]
- [Soberia/Lenovo-IdeaPad-Z500-Fan-Controller][Lenovo-IdeaPad-Z500-Fan-Controller]
- [Windows Drivers Reverse Engineering Methodology][windows-drivers-reverse-engineering-methodology]

[IdeaFan]: https://www.allstone.lt/ideafan/
[FanControl]: https://github.com/bitrate16/FanControl
[Lenovo-IdeaPad-Z500-Fan-Controller]: https://github.com/Soberia/Lenovo-IdeaPad-Z500-Fan-Controller
[windows-drivers-reverse-engineering-methodology]: https://voidsec.com/windows-drivers-reverse-engineering-methodology/
