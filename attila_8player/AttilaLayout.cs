// Attila UI layout files ("Version097") - schema-driven reader/writer for the Attila multi-player mod.
// Same design as eight_player_lobby\UiLayout.cs (Shogun 2 Version062). Field order taken from the game's own loader in
// the Attila DLL (component 113907D0, image 11393460, state 11394BF0, state image 11393720, anim 11392630, anim
// element 113929D0 + 113AB0E0); a file read and written back must be byte-identical (Roundtrip).
// PowerShell: Add-Type -Path AttilaLayout.cs
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

public sealed class AtValue {
	public string Name; public string Type; public object Data;
	public override string ToString() { return Name + ":" + Type; }
}

public sealed class AtNode {
	public string Kind;
	public readonly List<AtValue> Values = new List<AtValue>();
	public AtValue Get(string name) { foreach (var v in Values) if (v.Name == name) return v; return null; }
	public bool Has(string name) { return Get(name) != null; }
	public string Text(string name) {
		var v = Get(name); if (v == null) return null;
		return v.Type == "wstr" ? Encoding.Unicode.GetString((byte[])v.Data) : AttilaLayout.Latin1.GetString((byte[])v.Data);
	}
	public void SetText(string name, string s) {
		var v = Get(name);
		v.Data = v.Type == "wstr" ? Encoding.Unicode.GetBytes(s) : AttilaLayout.Latin1.GetBytes(s);
	}
	public long Num(string name) { var v = Get(name); return Convert.ToInt64(v.Data); }
	public void SetNum(string name, long n) {
		var v = Get(name);
		if (v.Type == "u8") v.Data = (byte)n; else if (v.Type == "u32") v.Data = (uint)n; else v.Data = (int)n;
	}
	public List<AtNode> Items(string name) { var v = Get(name); return v == null ? new List<AtNode>() : (List<AtNode>)v.Data; }
	public string Name { get { return Text("name"); } }
	public List<AtNode> Children { get { return Items("children"); } }
}

public static class AttilaLayout {
	public static readonly Encoding Latin1 = Encoding.GetEncoding(28591);
	public const string Magic = "Version097";

	// "name:type"; list:<kind> = u32 count + items; opt:<kind>:<flag> = record present when the earlier u8 <flag> != 0
	// (kind "unknown" = structure not worked out yet: reading stops with an error).
	static readonly Dictionary<string, string> Schemas = new Dictionary<string, string> {
		{ "component",
			"uid:u32 name:str class:str x:i32 y:i32 f0:u8 f1:u8 f2:u32 f3:u8 f4:u8 f5:u8 f6:u8 f7:u8 f8:u8 " +
			"tip0:wstr tip1:wstr dock:u32 dock_x:i32 dock_y:i32 f9:u8 default_state:u32 images:list:image " +
			"image_ref:u32 f10:u32 states:list:state properties:list:property f11:u32 anims:list:anim " +
			"children:list:component has_t:u8 t:opt:layout:has_t s0:str s1:str has_b:u8 b:opt:refs:has_b " +
			"has_c:u8 c:opt:sorted:has_c u0:u32 has_le:u8 le:opt:unknown:has_le g0:u8 g1:u8 g2:u8" },
		// child layout engine (113A0A50): type name, then List / HorizontalList data (11397A90); Table not used in 097 files
		{ "layout", "type:str refs:list:ref sx:i32 sy:i32 l0:u8 l1:u32 l2:u8 l3:u8 l4:u32" },
		{ "sorted", "c0:u32 refs:list:ref" },                          // 11396CC0
		{ "refs", "refs:list:ref" },                                   // 11396B50
		{ "ref", "v:u32" },
		{ "image", "uid:u32 path:str w:u32 h:u32 i0:u8" },
		{ "state",
			"uid:u32 name:str w:u32 h:u32 text:wstr tooltip:wstr s0:u32 s1:u32 s2:u32 s3:u32 s4:u32 s5:u8 " +
			"s6:wstr s7:wstr s8:str font:str s9:u32 s10:u32 s11:u32 colour:u32 font_category:str " +
			"p0:i32 p1:i32 p2:i32 p3:i32 b0:u8 b1:u8 b2:u8 b3:u8 shader:str v0:u32 v1:u32 v2:u32 v3:u32 " +
			"shader2:str v4:u32 v5:u32 v6:u32 v7:u32 images:list:state_image ox:i32 oy:i32 transitions:list:transition" },
		{ "state_image",
			"image:u32 x:i32 y:i32 w:u32 h:u32 colour:u32 m0:u8 m1:u8 m2:u8 m3:u32 mx:i32 my:i32 m4:u8 m5:u8 " +
			"m6:u32 m7:u32 m8:u32 mask:str n0:u32 n1:u32 n2:u32 n3:u32 n4:u32 n5:u32 n6:u32 n7:u32" },
		{ "transition", "t0:u32 t1:u32 t2:u32 t3:u32 entries:list:entry" },
		{ "entry", "v:u32 s0:str s1:str s2:str" },
		{ "property", "key:str value:str" },
		{ "anim", "name:str a0:u8 a1:u8 elements:list:anim_element tail:str" },
		{ "anim_element",
			"e0:u32 e1:u32 e2:u32 e3:u32 c0:u8 c1:u8 c2:u8 c3:u8 e4:u32 e5:u32 e6:u32 e7:u32 e8:u32 e9:u32 e10:u32 " +
			"e11:u32 e12:u32 e13:u32 s:str entries:list:entry last:u8" },
	};

	static readonly Dictionary<string, string[][]> Parsed = Schemas.ToDictionary(
		kv => kv.Key,
		kv => kv.Value.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries).Select(f => {
			int c = f.IndexOf(':'); return new[] { f.Substring(0, c), f.Substring(c + 1) };
		}).ToArray());

	sealed class Cursor {
		public byte[] B; public int P; public Stack<string> Where = new Stack<string>();
		public void Need(int n) { if (n < 0 || P + n > B.Length) throw new InvalidDataException("needs " + n + " more bytes"); }
		public byte U8() { Need(1); return B[P++]; }
		public uint U32() { Need(4); uint v = BitConverter.ToUInt32(B, P); P += 4; return v; }
		public byte[] Bytes(int n) { Need(n); var r = new byte[n]; Buffer.BlockCopy(B, P, r, 0, n); P += n; return r; }
		public int Len16() { Need(2); int v = BitConverter.ToUInt16(B, P); P += 2; return v; }
	}

	public static AtNode Read(byte[] data) {
		if (data.Length < Magic.Length || Latin1.GetString(data, 0, Magic.Length) != Magic) throw new InvalidDataException("not a " + Magic + " layout");
		var c = new Cursor { B = data, P = Magic.Length };
		AtNode root;
		try { root = ReadNode(c, "component"); }
		catch (Exception e) { throw new InvalidDataException(string.Format("{0} at 0x{1:X} ({2})", e.Message, c.P, string.Join(" / ", c.Where.Reverse())), e); }
		if (c.P != data.Length) throw new InvalidDataException(string.Format("{0} bytes left after the root component", data.Length - c.P));
		return root;
	}

	static AtNode ReadNode(Cursor c, string kind) {
		if (kind == "unknown") throw new InvalidDataException("record not worked out yet");
		var n = new AtNode { Kind = kind };
		foreach (var f in Parsed[kind]) {
			string name = f[0], type = f[1];
			object val;
			if (type == "u8") val = c.U8();
			else if (type == "u32") val = c.U32();
			else if (type == "i32") val = (int)c.U32();
			else if (type == "str") {
				val = c.Bytes(c.Len16());
				if (kind == "layout" && name == "type") { var tn = Latin1.GetString((byte[])val); if (tn != "List" && tn != "HorizontalList") throw new InvalidDataException("layout engine '" + tn + "' not worked out"); }
			}
			else if (type == "wstr") val = c.Bytes(c.Len16() * 2);
			else if (type.StartsWith("list:")) {
				uint count = c.U32();
				if (count > 200000) throw new InvalidDataException("list " + name + " claims " + count + " items");
				var items = new List<AtNode>((int)Math.Min(count, 1024));
				for (uint i = 0; i < count; i++) items.Add(ReadNode(c, type.Substring(5)));
				val = items;
			}
			else if (type.StartsWith("opt:")) {
				var p = type.Split(':');
				if ((byte)n.Get(p[2]).Data == 0) continue;
				c.Where.Push(name);
				val = ReadNode(c, p[1]);
				c.Where.Pop();
			}
			else throw new InvalidOperationException("bad schema type " + type);
			n.Values.Add(new AtValue { Name = name, Type = type, Data = val });
			if (name == "name" && type == "str") c.Where.Push(kind + " " + Latin1.GetString((byte[])val));
		}
		if (n.Has("name") && n.Get("name").Type == "str") c.Where.Pop();
		return n;
	}

	public static byte[] Write(AtNode root) {
		var ms = new MemoryStream();
		var w = new BinaryWriter(ms);
		w.Write(Latin1.GetBytes(Magic));
		WriteNode(w, root);
		w.Flush();
		return ms.ToArray();
	}

	static void WriteNode(BinaryWriter w, AtNode n) {
		foreach (var v in n.Values) {
			switch (v.Type) {
				case "u8": w.Write((byte)v.Data); break;
				case "u32": w.Write((uint)v.Data); break;
				case "i32": w.Write((int)v.Data); break;
				case "str": { var b = (byte[])v.Data; w.Write((ushort)b.Length); w.Write(b); break; }
				case "wstr": { var b = (byte[])v.Data; w.Write((ushort)(b.Length / 2)); w.Write(b); break; }
				default:
					if (v.Type.StartsWith("opt:")) { WriteNode(w, (AtNode)v.Data); break; }
					if (!v.Type.StartsWith("list:")) throw new InvalidOperationException("bad type " + v.Type);
					var items = (List<AtNode>)v.Data; w.Write((uint)items.Count); foreach (var it in items) WriteNode(w, it);
					break;
			}
		}
	}

	public static bool Roundtrip(byte[] data) {
		var again = Write(Read(data));
		return again.Length == data.Length && again.SequenceEqual(data);
	}

	public static IEnumerable<AtNode> Walk(AtNode root) {
		yield return root;
		foreach (var c in root.Children) foreach (var d in Walk(c)) yield return d;
	}
	public static AtNode Find(AtNode root, string name) { return Walk(root).FirstOrDefault(c => c.Name == name); }
	public static AtNode ParentOf(AtNode root, AtNode child) { return Walk(root).FirstOrDefault(c => c.Children.Contains(child)); }

	public static AtNode Copy(AtNode n) {
		var r = new AtNode { Kind = n.Kind };
		foreach (var v in n.Values) {
			object d = v.Data;
			if (d is byte[]) d = ((byte[])d).Clone();
			else if (d is AtNode) d = Copy((AtNode)d);
			else if (d is List<AtNode>) d = ((List<AtNode>)d).Select(Copy).ToList();
			r.Values.Add(new AtValue { Name = v.Name, Type = v.Type, Data = d });
		}
		return r;
	}

	public static string Tree(AtNode n, int maxDepth) { var sb = new StringBuilder(); Tree(sb, n, 0, maxDepth); return sb.ToString(); }
	static void Tree(StringBuilder sb, AtNode n, int depth, int maxDepth) {
		var st = n.Items("states");
		sb.AppendFormat("{0}{1} [{2}] ({3},{4}) {5}x{6} dock={7}({8},{9}) children={10}\n", new string(' ', depth * 2),
			n.Name, n.Text("class"), n.Num("x"), n.Num("y"), st.Count > 0 ? st[0].Num("w") : 0, st.Count > 0 ? st[0].Num("h") : 0,
			n.Num("dock"), n.Num("dock_x"), n.Num("dock_y"), n.Children.Count);
		if (depth < maxDepth) foreach (var c in n.Children) Tree(sb, c, depth + 1, maxDepth);
	}
}
