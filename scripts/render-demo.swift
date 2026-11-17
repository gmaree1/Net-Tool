// macOS AppKit renderer for eight synthetic Core2 UI previews in docs/images.
// Draws maintained illustrations of src/main.cpp layouts at 3x LCD resolution.
// macOS: swift scripts/render-demo.swift
import AppKit

let out = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("docs/images")
try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
let scale: CGFloat = 3
func color(_ h: String) -> NSColor {
    let n = UInt32(h, radix: 16)!
    return NSColor(srgbRed: CGFloat((n >> 16) & 255)/255, green: CGFloat((n >> 8) & 255)/255, blue: CGFloat(n & 255)/255, alpha: 1)
}
let bg="080e1c", card="18243e", white="ffffff", dim="8ca0c3", cyan="22d3ee", green="22c55e", yellow="facc15", orange="f97316", purple="a855f7", blue="3b82f6", pink="ec4899", teal="14b8a6"
func rect(_ x: CGFloat,_ y: CGFloat,_ w: CGFloat,_ h: CGFloat,_ c: String,_ r: CGFloat=0,_ stroke: String?=nil) {
    let p=NSBezierPath(roundedRect:NSRect(x:x,y:y,width:w,height:h),xRadius:r,yRadius:r)
    color(c).setFill(); p.fill()
    if let s=stroke { color(s).setStroke(); p.lineWidth=1; p.stroke() }
}
func line(_ pts: [(CGFloat,CGFloat)],_ c: String,_ width: CGFloat=1.2) {
    let p=NSBezierPath(); p.move(to:NSPoint(x:pts[0].0,y:pts[0].1))
    for pt in pts.dropFirst() {p.line(to:NSPoint(x:pt.0,y:pt.1))}
    color(c).setStroke(); p.lineWidth=width; p.stroke()
}
func ellipse(_ x: CGFloat,_ y: CGFloat,_ w: CGFloat,_ h: CGFloat,_ c: String,_ fill: Bool=false) {
    let p=NSBezierPath(ovalIn:NSRect(x:x,y:y,width:w,height:h)); p.lineWidth=1.2
    if fill {color(c).setFill(); p.fill()} else {color(c).setStroke(); p.stroke()}
}
func text(_ s: String,_ x: CGFloat,_ y: CGFloat,_ size: CGFloat=12,_ c: String=white,_ bold: Bool=false,_ align: String="left",_ maxW: CGFloat=320) {
    let font=NSFont(name:bold ? "Arial-BoldMT":"ArialMT",size:size)!
    let attrs: [NSAttributedString.Key:Any]=[.font:font,.foregroundColor:color(c)]
    var value=s
    while (value as NSString).size(withAttributes:attrs).width > maxW && value.count > 2 {value=String(value.dropLast(3))+".."}
    let width=(value as NSString).size(withAttributes:attrs).width
    let xx = align == "center" ? x-width/2 : align == "right" ? x-width : x
    (value as NSString).draw(at:NSPoint(x:xx,y:y),withAttributes:attrs)
}
func button(_ x: CGFloat,_ w: CGFloat,_ name: String,_ c: String,_ active: Bool=false) {
    rect(x,198,w,40,active ? c:card,8,active ? nil:c)
    text(name,x+w/2,211,12,active ? "000000":c,true,"center",w-6)
}
func tabs(_ selected: Int) {
    for (i,s) in ["NETWORK","SWITCH","TOOLS"].enumerated() {button(CGFloat(3+i*106),102,s,[cyan,yellow,orange][i],i==selected)}
}
func status() {
    rect(0,0,320,28,green); ellipse(8,8,12,12,"000000",true); ellipse(10,10,8,8,white,true)
    text("LINK 100M FULL",28,7,12,"000000",true); text("84%",312,7,12,"000000",true,"right")
}
func header() {
    rect(0,0,320,28,"0f1b2c"); rect(0,27,320,1,"253d51")
    ellipse(9,10,6,6,cyan,true);text("WI-FI SURVEY",23,7,12,white,true)
    rect(250,5,62,18,"0f3a49",5);text("2.4 GHz",281,9,9,cyan,false,"center")
}
func vlan(_ x: CGFloat,_ y: CGFloat,_ w: CGFloat) {
    rect(x,y,w,56,green,10);text("VLAN",x+w/2,y+4,12,"000000",false,"center");text("20",x+w/2,y+22,31,"000000",true,"center")
}
func info(_ x: CGFloat,_ y: CGFloat,_ w: CGFloat,_ label: String,_ value: String,_ accent: String) {
    rect(x,y,w,34,card,6);rect(x,y+5,3,24,accent);text(label,x+9,y+4,8,accent);text(value,x+9,y+17,12,white,true,"left",w-17)
}
func wifiIcon(_ x: CGFloat,_ y: CGFloat) {
    for r in [8.0,14.0,20.0] {let pts = stride(from:-0.7,through:0.7,by:0.05).map {t in (x+CGFloat(r*sin(t)),y-CGFloat(r*cos(t)))};line(pts,cyan)}
    ellipse(x-2,y-3,4,4,cyan,true)
}
func icon(_ i:Int,_ x:CGFloat,_ y:CGFloat,_ c:String) {
    switch i {
    case 0:
        rect(x-13,y-3,26,15,card,3,c);line([(x-7,y-3),(x-10,y-10)],c);line([(x+7,y-3),(x+10,y-10)],c)
        for dx in [-7,0,7] {ellipse(x+CGFloat(dx)-1,y+3,2,2,c,true)}
    case 1: ellipse(x-12,y-12,24,24,c);ellipse(x-5,y-12,10,24,c);ellipse(x-12,y-5,24,10,c)
    case 2:
        let pts:[(CGFloat,CGFloat)]=[(x-12,y+7),(x-3,y-6),(x+4,y+5),(x+12,y-7)];line(pts,c)
        for p in pts {ellipse(p.0-3,p.1-3,6,6,c,true)}
    case 3:
        rect(x-14,y-10,28,21,card,3,c);line([(x-13,y-4),(x+13,y-4)],c)
        for dx in [-9,-5] {ellipse(x+CGFloat(dx)-1,y-8,2,2,c,true)}
        ellipse(x-3,y-3,12,12,c);ellipse(x+1,y-3,4,12,c);ellipse(x-3,y+1,12,4,c)
    case 4:
        for j in 0..<2 {let yy=y-10+CGFloat(j*12);rect(x-13,yy,26,10,card,2,c);ellipse(x-9,yy+4,2,2,c,true);line([(x-3,yy+5),(x+9,yy+5)],c)}
    case 5:
        rect(x-14,y-8,28,17,card,3,c)
        for j in 0..<4 {rect(x-10+CGFloat(j*6),y-3,4,6,card,0,c)}
        line([(x+9,y-12),(x+12,y-8)],c);line([(x+8,y-7),(x+14,y-7)],c)
    case 6:
        for (dx,dy) in [(-12,-11),(2,-11),(-12,3)] {rect(x+CGFloat(dx),y+CGFloat(dy),11,11,card,0,c);rect(x+CGFloat(dx+3),y+CGFloat(dy+3),5,5,c)}
        for dx in [3,9] {for dy in [4,10] {rect(x+CGFloat(dx),y+CGFloat(dy),3,3,c)}}
    default:
        for j in 0..<3 {rect(x-12+CGFloat((j%2)*14),y-12+CGFloat((j/2)*14),10,10,card,2,c)}
        line([(x+7,y+2),(x+7,y+12)],c);line([(x+2,y+7),(x+12,y+7)],c)
    }
}
func render(_ name:String,_ draw:()->Void) throws {
    let image=NSImage(size:NSSize(width:960,height:720))
    image.lockFocusFlipped(true)
    NSGraphicsContext.current!.imageInterpolation = .high
    NSAffineTransform(scaleBy:scale).concat()
    rect(0,0,320,240,bg);draw();image.unlockFocus()
    let bitmap=NSBitmapImageRep(data:image.tiffRepresentation!)!
    try bitmap.representation(using:.png,properties:[:])!.write(to:out.appendingPathComponent(name+".png"))
    print("Rendered \(name).png")
}
extension NSAffineTransform {
    convenience init(scaleBy v:CGFloat) {self.init();self.scale(by:v)}
}
try render("network") {
    status();rect(4,30,210,56,card,8);text("IP ADDRESS",12,35,12,dim);text("192.0.2.42",12,49,20,cyan,true);text("/24  255.255.255.0",12,70,12)
    vlan(220,30,96);info(4,91,152,"GATEWAY","192.0.2.1",green);info(164,91,152,"DNS","192.0.2.10",blue)
    info(4,130,210,"DHCP SERVER / RELAY","192.0.2.5",orange);info(222,130,94,"LEASE","1d 6h",pink)
    rect(4,169,312,23,card,6);text("SWITCH",10,174,12,purple);text("DEMO-SW-01  Gi1/0/12",65,174,12);tabs(0)
}
try render("switch") {
    status();rect(4,30,312,62,card,8);text("DEMO-SW-01",12,38,13,white,true);text("Gi1/0/12",12,60,20,yellow,true);vlan(210,33,102)
    let rows=[("VLANS","native 20  voice 30",yellow),("MGMT IP","192.0.2.2",cyan),("MODEL","Demo access switch",pink),("VERSION","Demo firmware",blue),("SOURCE","CDP+LLDP  3s ago  full-dup",purple)]
    for (i,r) in rows.enumerated() {text(r.0,8,97+CGFloat(i*19),12,r.2);text(r.1,92,97+CGFloat(i*19),12,white,false,"left",224)};tabs(1)
}
try render("tools") {
    status();let colors=[green,blue,purple,teal,orange,pink,cyan,yellow]
    for i in 0..<8 {let x=CGFloat(6+(i%2)*156), y=CGFloat(34+(i/2)*39);rect(x,y,152,35,card,7,colors[i]);icon(i,x+76,y+17,colors[i])};tabs(2)
}
try render("extra-tools") {
    status();text("EXTRA TOOLS",12,41,20,white,true);text("Wireless discovery & diagnostics",12,68,12,dim)
    rect(6,94,308,78,card,12,"2b4763");rect(16,108,50,50,"0f3a49",10);wifiIcon(41,146)
    text("Wi-Fi scanner",80,111,20,white,true);text("SSIDs, signal & AP details",80,141,12,dim);text(">",298,121,14,cyan)
    text("2.4 GHz  /  No connection required",160,181,9,dim,false,"center");button(3,314,"BACK TO TOOLS",cyan)
}
try render("wifi-list") {
    header();text("4 access points",9,34,13,white,true);rect(253,32,61,27,"0f3a49",6);text("RESCAN",283,42,9,cyan,false,"center")
    text("Strongest first  |  2s ago  |  1/1",9,58,9,dim)
    let aps=[("Demo-Office","-48","6","WPA2 Personal","00:00:01",4),("Demo-Guest","-63","1","WPA2 Personal","00:00:02",3),("Demo-Lab","-72","11","WPA2 / WPA3","00:00:03",2),("Hidden network","-81","6","WPA2 Personal","00:00:04",1)]
    for (i,a) in aps.enumerated() {let y=CGFloat(72+i*31), c=i<2 ? green : i==2 ? yellow:orange;rect(6,y,308,29,card,6)
        for j in 0..<4 {rect(14+CGFloat(j*5),y+19-CGFloat(j*4),3,CGFloat(4+j*4),j<a.5 ? c:"2f3e54",1)}
        text(a.0,42,y+2,13,white,true);text(a.1+" dBm",292,y+2,12,c,false,"right");text("CH \(a.2)  \(a.3)  \(a.4)",42,y+18,8,dim);text(">",305,y+7,12,dim)
    };button(3,102,"BACK",cyan);button(109,102,"PREV",dim);button(215,102,"NEXT",dim)
}
func detailHeader() {header();rect(6,33,308,43,card,8);text("NETWORK NAME / SSID",14,37,9,cyan);text("Demo-Office",14,52,13,white,true)}
try render("wifi-identity") {
    detailHeader();text("Demo-Office",14,83,12)
    for (i,r) in [("BSSID / MAC","02:00:00:00:00:01"),("Security","WPA2 Personal"),("AP hostname","Not in scan data")].enumerated() {let y=CGFloat(118+i*21);text(r.0,12,y,12,dim);text(r.1,111,y,12,i==0 ? cyan:white)}
    text("SSID is the advertised name; IP/model need AP access.",160,184,9,dim,false,"center")
    button(3,102,"BACK",cyan);button(109,102,"IDENTITY",cyan,true);button(215,102,"RADIO",cyan)
}
try render("wifi-radio") {
    detailHeader();rect(6,81,116,79,card,8);text("-48",14,88,31,green,true);text("dBm",113,105,9,dim,false,"right");text("Excellent",14,127,12,green);rect(14,150,100,3,green,1)
    text("CHANNEL 6  /  2437 MHz",132,87,12,cyan);text("20 MHz  |  no 2nd channel",132,107,9);text("802.11 b/g/n",132,122,12);text("CC: US  |  WPS: No",132,145,9,dim)
    text("Pair: AES-CCMP   Group: AES-CCMP",12,167,9);text("Scan snapshot  /  2s ago  /  BACK > RESCAN to update",160,184,9,dim,false,"center")
    button(3,102,"BACK",cyan);button(109,102,"IDENTITY",cyan);button(215,102,"RADIO",cyan,true)
}
try render("ping") {
    status();text("PING GATEWAY",8,33,13,cyan,true)
    let rows=["#2  reply   2 ms   ttl 64","#3  reply   1 ms   ttl 64","#4  reply   1 ms   ttl 64","#5  reply   2 ms   ttl 64","#6  reply   1 ms   ttl 64","#7  reply   1 ms   ttl 64","#8  reply   1 ms   ttl 64","8/8 received, 0% loss","min 1  avg 1  max 2 ms"]
    for (i,s) in rows.enumerated() {text(s,8,50+CGFloat(i*16),12,i==8 ? white:green)}
    button(3,150,"BACK",green,true);button(159,76,"UP",cyan);button(241,76,"DOWN",cyan)
}
