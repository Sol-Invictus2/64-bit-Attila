using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

// Reader for Attila / Rome II "PFH4" packs.
// Header (28 bytes): "PFH4", u32 type, u32 packRefCount, u32 packRefSize,
//   u32 fileCount, u32 fileIndexSize, u32 timestamp.
// Index entry: u32 size, [u32 timestamp if type 0x40], null-terminated name.
// File data follows the index back to back.
public class Pfh4Entry {
    public string Name;
    public long Offset;
    public int Size;
}

public class Pfh4Pack {
    public uint Type;
    public List<string> PackRefs = new List<string>();
    public List<Pfh4Entry> Entries = new List<Pfh4Entry>();
    public string Path;

    public static Pfh4Pack Open(string path) {
        var p = new Pfh4Pack { Path = path };
        using (var fs = File.OpenRead(path))
        using (var br = new BinaryReader(fs)) {
            string magic = Encoding.ASCII.GetString(br.ReadBytes(4));
            if (magic != "PFH4") throw new Exception("not a PFH4 pack (" + magic + "): " + path);
            p.Type = br.ReadUInt32();
            int refCount = br.ReadInt32();
            int refSize = br.ReadInt32();
            int count = br.ReadInt32();
            int idxSize = br.ReadInt32();
            br.ReadUInt32();
            for (int i = 0; i < refCount; i++) p.PackRefs.Add(ReadCStr(br));
            long pos = 28 + refSize + idxSize;
            for (int i = 0; i < count; i++) {
                var e = new Pfh4Entry();
                e.Size = br.ReadInt32();
                if ((p.Type & 0x40) != 0) br.ReadUInt32();
                e.Name = ReadCStr(br);
                e.Offset = pos;
                pos += e.Size;
                p.Entries.Add(e);
            }
        }
        return p;
    }

    static string ReadCStr(BinaryReader br) {
        var ms = new MemoryStream();
        byte c;
        while ((c = br.ReadByte()) != 0) ms.WriteByte(c);
        return Encoding.UTF8.GetString(ms.ToArray());
    }

    public byte[] Read(Pfh4Entry e) {
        using (var fs = File.OpenRead(Path)) {
            fs.Seek(e.Offset, SeekOrigin.Begin);
            var b = new byte[e.Size];
            int r = 0;
            while (r < b.Length) { int n = fs.Read(b, r, b.Length - r); if (n <= 0) throw new EndOfStreamException(); r += n; }
            return b;
        }
    }

    // First n bytes of an entry (headers only, avoids reading whole meshes).
    public byte[] ReadHead(FileStream fs, Pfh4Entry e, int n) {
        n = Math.Min(n, e.Size);
        fs.Seek(e.Offset, SeekOrigin.Begin);
        var b = new byte[n];
        int r = 0;
        while (r < n) { int k = fs.Read(b, r, n - r); if (k <= 0) throw new EndOfStreamException(); r += k; }
        return b;
    }

    // Writes a new pack (type 3 = mod) from name/data pairs; no pack refs, no per-file timestamps.
    public static void Write(string path, uint type, List<KeyValuePair<string, byte[]>> files) {
        var idx = new MemoryStream();
        var iw = new BinaryWriter(idx);
        foreach (var f in files) { iw.Write(f.Value.Length); iw.Write(Encoding.UTF8.GetBytes(f.Key)); iw.Write((byte)0); }
        using (var fs = File.Create(path))
        using (var bw = new BinaryWriter(fs)) {
            bw.Write(Encoding.ASCII.GetBytes("PFH4"));
            bw.Write(type); bw.Write(0); bw.Write(0);
            bw.Write(files.Count); bw.Write((int)idx.Length);
            bw.Write((uint)(DateTimeOffset.UtcNow.ToUnixTimeSeconds()));
            bw.Write(idx.ToArray());
            foreach (var f in files) bw.Write(f.Value);
        }
    }

    public Pfh4Entry Find(string name) {
        foreach (var e in Entries) if (string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase)) return e;
        return null;
    }
}
