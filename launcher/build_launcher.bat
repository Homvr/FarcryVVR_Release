@echo off
rem Builds FC5VR_Launcher.exe (launcher\FC5VR_Launcher.cs, FC5VR_Report.cs) into launcher\out\. Uses the Build Tools'
rem C# compiler, or the one in Windows' .NET Framework 4 when the Build Tools are missing (C# 5: /langversion:5).
setlocal
cd /d "%~dp0"
set CSC="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if not exist %CSC% set CSC="%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist out mkdir out
%CSC% /nologo /target:winexe /optimize+ /platform:anycpu /langversion:5 /out:out\FC5VR_Launcher.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Core.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll FC5VR_Launcher.cs FC5VR_Report.cs || exit /b 1
echo built launcher\out\FC5VR_Launcher.exe
