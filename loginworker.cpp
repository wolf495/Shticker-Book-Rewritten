/*
 * Copyright (c) 2015-2016 Joshua Snyder
 * Distributed under the GNU GPL v3. For full terms see the file LICENSE
 *
 * This file is part of Shticker Book Rewritten.
 *
 * Shticker Book Rewritten is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shticker Book Rewritten is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shticker Book Rewritten.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "loginworker.h"
#include "twofactorwindow.h"
#include "globaldefines.h"
#include "libraries/qslog/QsLog.h"

#include <QUrlQuery>
#include <QProcessEnvironment>
#include <QDir>


LoginWorker::LoginWorker(QObject *parent) : QObject(parent)
{
    timer = new QTimer();
    timer->setSingleShot(true);
    connect(timer, SIGNAL(timeout()), this, SLOT(timerFinished()));
}

QByteArray LoginWorker::loginApiWorker(QByteArray postData)
{
    networkManager = new QNetworkAccessManager(this);
    //setup the headers for our API request
    QNetworkRequest request(QUrl("https://www.toontownrewritten.com/api/login?format=json"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "text/plain");

    reply = networkManager->post(request, postData);  //initiate the file download
    connect(reply, SIGNAL(finished()), &waitForFinished, SLOT(quit())); //when the file is finished stop the event loop
    waitForFinished.exec(); //start the event loop

    QByteArray apiReply;
    apiReply = reply->readAll();

    //schedule to erase the reply
    reply->deleteLater();
    networkManager->deleteLater();

    return apiReply;
}

void LoginWorker::initiateLogin(QString username, QString password)
{
    //let the user know we are starting the login
    emit sendMessage("Sending credentials to the TTR Servers");

    //setup the initial POST parameters for the login API
    QByteArray postData;
    QUrlQuery postParameters;
    postParameters.addQueryItem("username", username);
    postParameters.addQueryItem("password", password);
    postData.append(postParameters.toString());

    QByteArray replyData;

    //send the initial login request
    replyData = loginApiWorker(postData);

    //reset the querry parameters
    postData.clear();
    postParameters.clear();

    parseError = new QJsonParseError();    //container to hold any errors when parsing the json document

    jsonDocument = QJsonDocument::fromJson(replyData, parseError);   //parse the downloaded file into a JSON array
    jsonObject = jsonDocument.object();

    authenticationLoop();
}

void LoginWorker::authenticationLoop()
{
    //if the login gets rejected for whatever reason
    if(jsonObject["success"].toString() == "false")
    {
        //display the login fail message
        emit sendMessage(jsonObject["banner"].toString());
        QLOG_INFO() << "Login failed:" << jsonObject["banner"].toString();

        //let the main window know it failed to enable login again
        emit authenticationFailed();

        return;
    }
    //if the login required 2 factor authentication
    else if(jsonObject["success"].toString() == "partial")
    {
        authToken = jsonObject["responseToken"].toString();

        startTwoFactorAuthentication();
    }
    else if(jsonObject["success"].toString() == "delayed")
    {
        //when it isn't quite ready to login yet check again every 0.5 seconds
        QLOG_INFO() << "Waiting in queue. ETA:" << jsonObject["eta"].toString() << ", Position in line:" << jsonObject["position"].toString();
        sendMessage(QString("Waiting in queue. ETA: ") + jsonObject["eta"].toString() + QString(", Position in line: ") + jsonObject["position"].toString());

        lineToken = jsonObject["queueToken"].toString();

        timer->start(500);
    }
    else if(jsonObject["success"].toString() == "true")
    {
        QString playCookie = jsonObject["cookie"].toString();
        QString gameServer = jsonObject["gameserver"].toString();

        QLOG_INFO() << "Authenticated fully and starting game!";
        QLOG_INFO() << "Play cookie:" << playCookie << "Game server:" << gameServer;
        emit sendMessage("Authentication complete. Starting game now.");

        //start the game now that we have the play cookie and server info
        startGame(playCookie, gameServer);
    }
    else
    {
        QLOG_INFO() << "Unable to authenticate.  Error:" << jsonObject["banner"].toString();
        emit sendMessage(jsonObject["banner"].toString());

        //let the main window know it failed to enable login again
        emit authenticationFailed();
    }
}

void LoginWorker::receiveToken(QString token)
{
    receivedToken = token;
    //stop the loop now that we have the token and allow the login worker to continue
    waitForFinished.exit();
}

void LoginWorker::startTwoFactorAuthentication()
{
    QByteArray replyData;
    QByteArray postData;
    QUrlQuery postParameters;

    //run as a loop to keep requesting a valid token until one is provided or the user gives up or the API decides to kick us
    while(jsonObject["success"].toString() == "partial")
    {
        emit sendMessage(jsonObject["banner"].toString());

        QLOG_INFO() << "Partial authentication: starting 2 factor authentication.";


        TwoFactorWindow *twoFactorWindow = new TwoFactorWindow(jsonObject["banner"].toString());
        twoFactorWindow->show();

        connect(twoFactorWindow, SIGNAL(sendToken(QString)), this, SLOT(receiveToken(QString)));

        waitForFinished.exec(); //wait for the user to provide a token

        //if the user decides to cancel the login
        if(receivedToken == "cancel")
        {
            emit sendMessage("Cancelling login.");
            QLOG_DEBUG() << "Cancelling two factor authentication\n";
            return;
        }

        //send a new API request with the provided token
        postParameters.addQueryItem("appToken", receivedToken);
        postParameters.addQueryItem("authToken", authToken);
        postData.append(postParameters.toString());

        replyData = loginApiWorker(postData);

        //reset the querry parameters
        postData.clear();
        postParameters.clear();

        jsonDocument = QJsonDocument::fromJson(replyData, parseError);   //parse the downloaded file into a JSON array
        jsonObject = jsonDocument.object();
    }
    if(jsonObject["success"].toString() != "false")
    {
        QLOG_INFO() << "Two Factor authentication complete";
    }

    //go back to the main loop now that we are authenticated
    authenticationLoop();
}

void LoginWorker::timerFinished()
{
    QLOG_INFO() << "Queued authentication: checking where we stand in line again.";

    //check the login API again to see if we are ready yet
    QByteArray replyData;
    QByteArray postData;
    QUrlQuery postParameters;

    //send a new API request with our line token
    postParameters.addQueryItem("queueToken", lineToken);
    postData.append(postParameters.toString());

    replyData = loginApiWorker(postData);

    postData.clear();
    postParameters.clear();

    jsonDocument = QJsonDocument::fromJson(replyData, parseError);   //parse the downloaded file into a JSON array
    jsonObject = jsonDocument.object();

    //go back to the main login loop to check where we stand in line
    authenticationLoop();
}

void LoginWorker::startGame(QString cookie, QString gameServer)
{
    QLOG_DEBUG() << "Starting game!\n";

    //setup the paths
    QString engineFile = FILES_PATH + ENGINE_FILENAME;
    QString gamePath = FILES_PATH;

    gameProcess = new QProcess();

    gameProcess->setWorkingDirectory(gamePath);
    QDir::setCurrent(gamePath);

    //set the environment variables for the engine
    QProcessEnvironment gameEnvironment = QProcessEnvironment::systemEnvironment();
    gameEnvironment.insert("TTR_PLAYCOOKIE", cookie);
    gameEnvironment.insert("TTR_GAMESERVER", gameServer);
    gameProcess->setProcessEnvironment(gameEnvironment);

    connect(gameProcess, SIGNAL(started()), this, SLOT(gameHasStarted()));
    connect(gameProcess, SIGNAL(finished(int)), this, SLOT(gameHasFinished(int)));

    //start the game
    gameProcess->start(engineFile);
    gameProcess->waitForStarted(30000);
}

void LoginWorker::gameHasStarted()
{
    emit gameStarted();
}

void LoginWorker::gameHasFinished(int)
{
    emit gameFinished();
}