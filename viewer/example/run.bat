@echo off
REM Build + run the example JVM to test the vmhook viewer against.
REM Leave this window running, then attach the viewer to the java.exe it spawns.
setlocal
cd /d "%~dp0"
echo Compiling com.example.demo.ExampleApp ...
javac -d out com\example\demo\ExampleApp.java || goto :err
echo Launching ExampleApp (Ctrl+C to stop). Attach the vmhook viewer to this JVM.
java -cp out com.example.demo.ExampleApp
goto :eof
:err
echo Build failed. Is the JDK's javac on PATH?
pause
