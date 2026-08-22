/*
 * To change this template, choose Tools | Templates
 * and open the template in the editor.
 */

package org.murillo.mscontrol;

import java.net.InetAddress;
import java.net.UnknownHostException;

/**
 * Sous-réseau IP, IPv4 comme IPv6.
 *
 * <p>La version d'origine raisonnait en <b>entier 32 bits</b> : les adresses
 * étaient lues par {@code Inet4Address.getByName()} — qui LÈVE sur une adresse
 * v6 — et le préfixe calculé par décalage sur un {@code int}. Une adresse IPv6
 * fait 128 bits : aucune des deux hypothèses ne survit. La comparaison se fait
 * donc désormais <b>octet par octet sur le tableau d'adresse</b>, ce qui vaut
 * pour 4 comme pour 16 octets et n'impose aucune borne de longueur.</p>
 *
 * <p>Règle qui n'existait pas avant et qu'il faut poser : <b>deux familles
 * différentes ne se contiennent jamais</b>. Une adresse v6 n'est pas « dans »
 * un sous-réseau v4, même si ses quatre premiers octets coïncidaient.</p>
 *
 * @author Sergio
 */
public class SubNetInfo {

    //Create private subnets
    private final static SubNetInfo loopback = new SubNetInfo(new byte[]{127,0,0,1}     ,8);
    private final static SubNetInfo classA   = new SubNetInfo(new byte[]{10,0,0,1}      ,8);
    private final static SubNetInfo classB   = new SubNetInfo(new byte[]{(byte)172,16,0,1}    ,12);
    private final static SubNetInfo classC   = new SubNetInfo(new byte[]{(byte)192,(byte)168,0,0}   ,16);
    //100.64.0.0/10 : NAT opérateur (RFC 6598) — même symptôme qu'une RFC 1918
    private final static SubNetInfo cgnat    = new SubNetInfo(new byte[]{100,64,0,0}    ,10);
    //169.254.0.0/16 : link-local v4 (RFC 3927), jamais routable jusqu'au serveur
    private final static SubNetInfo linkV4   = new SubNetInfo(new byte[]{(byte)169,(byte)254,0,0}   ,16);

    //IPv6. fc00::/7 (ULA, RFC 4193) est l'analogue du RFC 1918 ; fe80::/10 est
    //le link-local ; ::1/128 la loopback.
    private final static SubNetInfo ula      = new SubNetInfo(new byte[]{(byte)0xfc,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} ,7);
    private final static SubNetInfo linkV6   = new SubNetInfo(new byte[]{(byte)0xfe,(byte)0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0} ,10);
    private final static SubNetInfo loopV6    = new SubNetInfo(new byte[]{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1} ,128);

    /**
     * L'adresse est-elle privée, donc plausiblement derrière un NAT ?
     *
     * <p>En IPv6 il n'y a pas de NAT : les plages v6 listées ici ne sont pas
     * « nattables », elles sont simplement <b>non routables jusqu'au serveur</b>,
     * ce qui justifie le même traitement — ne pas y émettre le média.</p>
     */
    public static boolean isPrivate(String ip) throws UnknownHostException {
        //Get address bytes. InetAddress, et non Inet4Address : ce dernier lève
        //sur une adresse v6 au lieu de la classer.
        byte[] addr = InetAddress.getByName(ip).getAddress();

        //Check
        return loopback.contains(addr) || classA.contains(addr) || classB.contains(addr)
            || classC.contains(addr)   || cgnat.contains(addr)  || linkV4.contains(addr)
            || ula.contains(addr)      || linkV6.contains(addr) || loopV6.contains(addr);
    }


    private String subnet;
    private int mask;
    //Adresse de base du préfixe, en octets : c'est ce qui remplace l'entier
    //32 bits et ce qui rend la classe utilisable en IPv6.
    private byte[] base;

    public SubNetInfo(String cidr) throws UnknownHostException {
        try {
            //Split
            String[] info = cidr.split("/");
            //Split
            subnet = info[0];
            mask = Integer.parseInt(info[1]);
        } catch (Exception e) {
            throw new UnknownHostException(cidr);
        }
        //Get address bytes
        base = getBase(InetAddress.getByName(subnet).getAddress(),mask);
    }

    public SubNetInfo(String subnet,int mask) throws UnknownHostException {
        //Store values
        this.subnet = subnet;
        this.mask = mask;
        //Get address bytes
        base = getBase(InetAddress.getByName(subnet).getAddress(),mask);
    }

    public SubNetInfo(byte[] addr,int mask) {
        //Store values
        this.subnet = formatAddress(addr);
        this.mask = mask;
        //Get address bytes
        base = getBase(addr,mask);
    }

    public boolean contains(String ip) throws UnknownHostException {
        //Get addres
        byte[] addr = InetAddress.getByName(ip).getAddress();
        //Get bytes
        return contains(addr);
    }

    public boolean contains(byte[] addr) {
        //Deux familles différentes ne se contiennent jamais : une adresse v6
        //n'est pas « dans » un sous-réseau v4, quels que soient ses octets.
        if (addr==null || base==null || addr.length!=base.length)
            return false;

        //Compare les `mask` premiers bits, octet plein par octet plein puis
        //bits restants. Aucune borne à 32 : c'est tout l'objet du changement.
        int fullBytes = mask / 8;
        int leftBits  = mask % 8;

        for (int i=0; i<fullBytes; i++)
            if (addr[i]!=base[i])
                return false;

        //fullBytes<length : un masque plus long que l'adresse (33 bits sur une
        //v4, par exemple) ne doit pas sortir du tableau.
        if (leftBits>0 && fullBytes<addr.length) {
            int m = (0xFF << (8-leftBits)) & 0xFF;
            if ((addr[fullBytes] & m) != (base[fullBytes] & m))
                return false;
        }

        return true;
    }

    /** Adresse de base : l'adresse donnée, bits hors préfixe mis à zéro. */
    private static byte[] getBase(byte[] b,int mask) {
        if (b==null)
            return null;

        byte[] res = new byte[b.length];

        //Check mask
        if (mask<=0)
            //Matches everything
            return res;

        int fullBytes = mask / 8;
        int leftBits  = mask % 8;

        for (int i=0; i<b.length; i++) {
            if (i<fullBytes)
                res[i] = b[i];
            else if (i==fullBytes && leftBits>0)
                res[i] = (byte)(b[i] & ((0xFF << (8-leftBits)) & 0xFF));
            else
                res[i] = 0;
        }

        return res;
    }

    /** Forme textuelle d'une adresse brute, quelle que soit sa famille. */
    private static String formatAddress(byte[] addr) {
        try {
            return InetAddress.getByAddress(addr).getHostAddress();
        } catch (UnknownHostException e) {
            //Longueur d'adresse inconnue : on ne peut rien en dire de mieux
            return "";
        }
    }

    @Override
    public String toString() {
        return subnet + "/" + mask ;
    }
}
