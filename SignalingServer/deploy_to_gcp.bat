@echo off
setlocal

echo INFO: Checking for Google Cloud SDK...
where gcloud >nul 2>nul
if errorlevel 1 goto NO_GCLOUD

echo INFO: Detecting GCP Project...
for /f "tokens=*" %%i in ('gcloud config get-value project 2^>nul') do set PROJ=%%i

if "%PROJ%"=="" goto NO_PROJECT
if "%PROJ%"=="(unset)" goto NO_PROJECT

echo SUCCESS: Using Project %PROJ%

echo INFO: Building container in the cloud...
gcloud builds submit --tag gcr.io/%PROJ%/gupt-signal-server
if errorlevel 1 goto BUILD_FAILED

echo INFO: Deploying to Google Cloud Run...
gcloud run deploy gupt-signal-server --image gcr.io/%PROJ%/gupt-signal-server --platform managed --region us-central1 --allow-unauthenticated --port 8080
if errorlevel 1 goto DEPLOY_FAILED

echo.
echo ========================================================
echo SUCCESS: DEPLOYMENT COMPLETE!
echo ========================================================
echo 1. Copy the Service URL above.
echo 2. Update Launcher/main.cpp with the URL.
echo 3. Rebuild Gupt.exe.
echo ========================================================
pause
exit /b

:NO_GCLOUD
echo ERROR: Google Cloud SDK is not installed.
pause
exit /b

:NO_PROJECT
echo ERROR: No GCP Project selected. Run gcloud config set project first.
pause
exit /b

:BUILD_FAILED
echo ERROR: Docker build failed.
pause
exit /b

:DEPLOY_FAILED
echo ERROR: Deployment failed.
pause
exit /b
