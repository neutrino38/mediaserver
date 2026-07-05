/*
 * To change this template, choose Tools | Templates
 * and open the template in the editor.
 */

package org.murillo.mscontrol;

import java.net.URI;
import java.net.URISyntaxException;
import java.util.logging.Level;
import java.util.logging.Logger;
import org.murillo.MediaServer.XmlRpcEventManager;

/**
 *
 * @author Sergio
 */
public class MediaServerEventQueue implements XmlRpcEventManager.Listener, Runnable{
    public interface Listener {
        public abstract void onPlayerEndOfStream(URI sessUri,URI playerUri);
        //Événements de cycle de vie ajoutés (contrat de fil partagé avec le serveur
        //C++ JSR309Event::Events). Voir README de l'API pour la table des codes.
        public abstract void onPlayerStarted(URI sessUri,URI playerUri);                         //type=3
        public abstract void onRecorderStarted(URI sessUri,URI recorderUri);                     //type=4
        public abstract void onRecorderStopped(URI sessUri,URI recorderUri,int reason);          //type=5
        public abstract void onEndpointDisconnected(URI sessUri,int endpointId,int media,int role); //type=6
    }
    
    private final XmlRpcEventManager em;
    private Listener listener;
    private Thread thread;
    private final String url;

    @Override
    public void run() {
        try{
            //Connect
            em.Connect(url, this);
        } catch(Exception e) {
            //Send error
            onError();
        }
    }

    public void start(){
        //Create thread and start
        thread.start();
    }

    public void stop() {
        //Stop thread
        thread.stop();
    }
    

    MediaServerEventQueue(String url) {
        //Store url
        this.url = url;
        //Create event manager
        em = new XmlRpcEventManager();
        //Create thread;
        thread = new Thread(this);
    }

    public void setListener(Listener listener){
        this.listener = listener;
    }

    @Override
    public void onConnect() {
        //
    }

    @Override
    public void onError() {
        //
    }

    @Override
    public void onDisconnect() {
        //
    }

    @Override
    public void onEvent(Object result) {
        //Check listener
        if (listener==null)
            //Exit
            return;

        //Convert to array
        Object[] arr = (Object[]) result;
        //Get type
        Integer type = (Integer) arr[0];
        
        //Depending on the type
        switch(type)
        {
            case 1:
                try {
                    String sessTag = (String) arr[1];
                    String playerTag = (String) arr[2];
                    //PlayerEndOfFile
                    listener.onPlayerEndOfStream(new URI(sessTag), new URI(playerTag.replace("/player","")));
                } catch (URISyntaxException ex) {
                    Logger.getLogger(MediaServerEventQueue.class.getName()).log(Level.SEVERE, null, ex);
                }
                break;
            case 3:
                try {
                    String sessTag = (String) arr[1];
                    String playerTag = (String) arr[2];
                    //PlayerStarted
                    listener.onPlayerStarted(new URI(sessTag), new URI(playerTag.replace("/player","")));
                } catch (URISyntaxException ex) {
                    Logger.getLogger(MediaServerEventQueue.class.getName()).log(Level.SEVERE, null, ex);
                }
                break;
            case 4:
                try {
                    String sessTag = (String) arr[1];
                    String recorderTag = (String) arr[2];
                    //RecorderStarted
                    listener.onRecorderStarted(new URI(sessTag), new URI(recorderTag.replace("/recorder","")));
                } catch (URISyntaxException ex) {
                    Logger.getLogger(MediaServerEventQueue.class.getName()).log(Level.SEVERE, null, ex);
                }
                break;
            case 5:
                try {
                    String sessTag = (String) arr[1];
                    String recorderTag = (String) arr[2];
                    Integer reason = (Integer) arr[3];
                    //RecorderStopped (reason : 0=explicite, 1=durée max, 2=silence, 3=DTMF)
                    listener.onRecorderStopped(new URI(sessTag), new URI(recorderTag.replace("/recorder","")), reason);
                } catch (URISyntaxException ex) {
                    Logger.getLogger(MediaServerEventQueue.class.getName()).log(Level.SEVERE, null, ex);
                }
                break;
            case 6:
                try {
                    String sessTag = (String) arr[1];
                    Integer endpointId = (Integer) arr[2];
                    Integer media = (Integer) arr[3];
                    Integer role = (Integer) arr[4];
                    //EndpointDisconnected (timeout RTP)
                    listener.onEndpointDisconnected(new URI(sessTag), endpointId, media, role);
                } catch (URISyntaxException ex) {
                    Logger.getLogger(MediaServerEventQueue.class.getName()).log(Level.SEVERE, null, ex);
                }
                break;
        }
    }
}
