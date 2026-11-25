////////////////////////////////////////////////////////////////////////////
//	Module 		: InstanceRecord.cpp
//	Description : Per-item configuration record (WEAPON_ENGINE_PLAN.md 4.1, change 2a)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "InstanceRecord.h"

// Body layout, version 1 (all integers little endian, strings zero terminated):
//   u16 override count, then (key, value) per override
//   u16 next part id
//   u8  part count, then per part:
//       u16 id, u16 parent, string mount, f32 pos,
//       u8 prop count, then (key, value) per prop,
//       string section, u8 condition,
//       u8 upgrade count, then string per upgrade,
//       u16 blob size, then the bytes,
//       string paintjob
//   u8  runtime present (0/1); if 1: u8 fire mode, u8 gl mode, u16 round count, then u8 per round
// Any change to this layout bumps VERSION.

namespace
{
	struct writer
	{
		xr_vector<u8>& out;
		explicit writer(xr_vector<u8>& o) : out(o) {}

		void bytes(const void* p, u32 n)
		{
			const u8* b = static_cast<const u8*>(p);
			out.insert(out.end(), b, b + n);
		}
		void u8_(u8 v) { out.push_back(v); }
		void u16_(u16 v) { bytes(&v, sizeof(v)); }
		void f32_(float v) { bytes(&v, sizeof(v)); }
		void str(const shared_str& s)
		{
			const char* c = s.c_str() ? s.c_str() : "";
			bytes(c, xr_strlen(c) + 1);
		}
		void props(const InstanceProps& v)
		{
			for (const SInstanceProp& p : v)
			{
				str(p.key);
				str(p.value);
			}
		}
	};

	// Bounded reader: every read checks the remaining size; the first failure sticks
	struct reader
	{
		const u8* p;
		const u8* end;
		bool ok = true;
		LPCSTR error = nullptr;

		reader(const u8* data, u32 size) : p(data), end(data + size) {}

		bool fail(LPCSTR why)
		{
			if (ok)
			{
				ok = false;
				error = why;
			}
			return false;
		}
		bool bytes(void* dst, u32 n)
		{
			if (!ok) return false;
			if (u32(end - p) < n) return fail("body ends early");
			CopyMemory(dst, p, n);
			p += n;
			return true;
		}
		u8 u8_() { u8 v = 0; bytes(&v, sizeof(v)); return v; }
		u16 u16_() { u16 v = 0; bytes(&v, sizeof(v)); return v; }
		float f32_()
		{
			float v = 0.f;
			bytes(&v, sizeof(v));
			if (ok && !_valid(v)) fail("invalid number");
			return ok ? v : 0.f;
		}
		shared_str str()
		{
			if (!ok) return shared_str();
			const u8* z = p;
			while (z < end && *z) ++z;
			if (z == end) { fail("unterminated string"); return shared_str(); }
			if (u32(z - p) > CSE_InstanceRecord::MAX_STRING) { fail("string too long"); return shared_str(); }
			// Empty strings read back as null, the form an unset shared_str has, so a round trip compares equal
			shared_str s = (z == p) ? shared_str() : shared_str(reinterpret_cast<LPCSTR>(p));
			p = z + 1;
			return s;
		}
		void props(InstanceProps& v, u32 count)
		{
			v.clear();
			v.reserve(count);
			for (u32 i = 0; i < count && ok; ++i)
			{
				SInstanceProp prop;
				prop.key = str();
				prop.value = str();
				if (ok) v.push_back(prop);
			}
		}
	};

	// One representation for "empty": the null shared_str
	shared_str norm(LPCSTR s)
	{
		return (s && *s) ? shared_str(s) : shared_str();
	}

	bool str_ok(LPCSTR s)
	{
		return s && *s && xr_strlen(s) <= CSE_InstanceRecord::MAX_STRING;
	}
}

bool SInstancePart::operator==(const SInstancePart& o) const
{
	return id == o.id && parent == o.parent && mount == o.mount && pos == o.pos && props == o.props &&
		section == o.section && condition == o.condition && upgrades == o.upgrades && blob == o.blob &&
		paintjob == o.paintjob;
}

bool CSE_InstanceRecord::empty() const
{
	return !is_opaque() && overrides.empty() && parts.empty() && !runtime.valid && next_part_id == 1;
}

void CSE_InstanceRecord::clear()
{
	overrides.clear();
	next_part_id = 1;
	parts.clear();
	runtime = SInstanceRuntime();
	m_opaque.clear();
}

bool CSE_InstanceRecord::operator==(const CSE_InstanceRecord& o) const
{
	return overrides == o.overrides && next_part_id == o.next_part_id && parts == o.parts &&
		runtime == o.runtime && m_opaque == o.m_opaque;
}

void CSE_InstanceRecord::write_body(xr_vector<u8>& out) const
{
	writer w(out);
	w.u16_(u16(overrides.size()));
	w.props(overrides);
	w.u16_(next_part_id);
	w.u8_(u8(parts.size()));
	for (const SInstancePart& part : parts)
	{
		w.u16_(part.id);
		w.u16_(part.parent);
		w.str(part.mount);
		w.f32_(part.pos);
		w.u8_(u8(part.props.size()));
		w.props(part.props);
		w.str(part.section);
		w.u8_(part.condition);
		w.u8_(u8(part.upgrades.size()));
		for (const shared_str& u : part.upgrades)
			w.str(u);
		w.u16_(u16(part.blob.size()));
		if (!part.blob.empty())
			w.bytes(part.blob.data(), u32(part.blob.size()));
		w.str(part.paintjob);
	}
	w.u8_(runtime.valid ? 1 : 0);
	if (runtime.valid)
	{
		w.u8_(runtime.fire_mode);
		w.u8_(runtime.gl_mode);
		w.u16_(u16(runtime.rounds.size()));
		if (!runtime.rounds.empty())
			w.bytes(runtime.rounds.data(), u32(runtime.rounds.size()));
	}
}

bool CSE_InstanceRecord::fits() const
{
	if (overrides.size() > MAX_OVERRIDES || parts.size() > MAX_PARTS || runtime.rounds.size() > MAX_ROUNDS)
		return false;
	for (const SInstancePart& part : parts)
		if (part.props.size() > MAX_PROPS || part.upgrades.size() > MAX_UPGRADES || part.blob.size() > MAX_BLOB)
			return false;
	return true;
}

u32 CSE_InstanceRecord::byte_size() const
{
	if (is_opaque())
		return u32(m_opaque.size());
	if (empty())
		return 1;
	xr_vector<u8> body;
	write_body(body);
	return 1 + 2 + u32(body.size());
}

void CSE_InstanceRecord::write(NET_Packet& P) const
{
	if (is_opaque())
	{
		P.w(m_opaque.data(), u32(m_opaque.size()));
		return;
	}
	if (empty())
	{
		P.w_u8(0);
		return;
	}

	xr_vector<u8> body;
	if (fits())
		write_body(body);
	if (body.empty() || body.size() > MAX_BYTES)
	{
		// The editing functions enforce the caps; this only triggers if code filled the vectors directly
		Msg("! [instance record] record exceeds its limits (%u bytes), saved empty", u32(body.size()));
		P.w_u8(0);
		return;
	}
	P.w_u8(VERSION);
	P.w_u16(u16(body.size()));
	P.w(body.data(), u32(body.size()));
}

void CSE_InstanceRecord::read(NET_Packet& P)
{
	clear();
	if (P.r_elapsed() < 1)
		return;

	const u8 version = P.r_u8();
	if (version == 0)
		return;

	if (P.r_elapsed() < 2)
	{
		Msg("! [instance record] version %u without a size, ignored", version);
		P.r_advance(P.r_elapsed());
		return;
	}
	const u16 size = P.r_u16();
	if (P.r_elapsed() < size)
	{
		Msg("! [instance record] body of %u bytes but only %u left, ignored", size, P.r_elapsed());
		P.r_advance(P.r_elapsed());
		return;
	}

	xr_vector<u8> body(size);
	if (size)
		P.r(body.data(), size);

	if (version > VERSION)
	{
		// From a newer build: keep it exactly as it is so it survives a save here
		writer w(m_opaque);
		w.u8_(version);
		w.u16_(size);
		if (size)
			w.bytes(body.data(), size);
		Msg("* [instance record] version %u is newer than %u; kept unchanged and read-only", version, u32(VERSION));
		return;
	}

	if (!read_body(body.data(), size))
		clear();
}

bool CSE_InstanceRecord::read_body(const u8* data, u32 size)
{
	reader r(data, size);

	const u16 override_count = r.u16_();
	if (override_count > MAX_OVERRIDES) r.fail("too many overrides");
	r.props(overrides, override_count);
	next_part_id = r.u16_();

	const u8 part_count = r.u8_();
	if (part_count > MAX_PARTS) r.fail("too many parts");
	parts.reserve(part_count);
	for (u32 i = 0; i < part_count && r.ok; ++i)
	{
		SInstancePart part;
		part.id = r.u16_();
		part.parent = r.u16_();
		part.mount = r.str();
		part.pos = r.f32_();
		const u8 prop_count = r.u8_();
		if (prop_count > MAX_PROPS) r.fail("too many props");
		r.props(part.props, prop_count);
		part.section = r.str();
		part.condition = r.u8_();
		const u8 upgrade_count = r.u8_();
		if (upgrade_count > MAX_UPGRADES) r.fail("too many upgrades");
		for (u32 u = 0; u < upgrade_count && r.ok; ++u)
			part.upgrades.push_back(r.str());
		const u16 blob_size = r.u16_();
		if (blob_size > MAX_BLOB) r.fail("blob too large");
		if (r.ok && blob_size)
		{
			part.blob.resize(blob_size);
			r.bytes(part.blob.data(), blob_size);
		}
		part.paintjob = r.str();
		if (r.ok && (part.id == 0 || part.id >= next_part_id)) r.fail("part id out of range");
		if (r.ok) parts.push_back(part);
	}

	if (r.u8_())
	{
		runtime.valid = true;
		runtime.fire_mode = r.u8_();
		runtime.gl_mode = r.u8_();
		const u16 round_count = r.u16_();
		if (round_count > MAX_ROUNDS) r.fail("too many rounds");
		if (r.ok && round_count)
		{
			runtime.rounds.resize(round_count);
			r.bytes(runtime.rounds.data(), round_count);
		}
	}

	if (r.ok && r.p != r.end)
		r.fail("unexpected bytes after the body");

	if (!r.ok)
		Msg("! [instance record] damaged record (%s), ignored", r.error);
	return r.ok;
}

LPCSTR CSE_InstanceRecord::get_override(LPCSTR key) const
{
	for (const SInstanceProp& p : overrides)
		if (!xr_strcmp(p.key, key))
			return p.value.c_str();
	return nullptr;
}

bool CSE_InstanceRecord::set_override(LPCSTR key, LPCSTR value)
{
	if (is_opaque() || !str_ok(key) || !value || xr_strlen(value) > MAX_STRING)
		return false;
	for (SInstanceProp& p : overrides)
		if (!xr_strcmp(p.key, key))
		{
			p.value = norm(value);
			return true;
		}
	if (overrides.size() >= MAX_OVERRIDES)
		return false;
	SInstanceProp p;
	p.key = key;
	p.value = norm(value);
	overrides.push_back(p);
	if (byte_size() > MAX_BYTES)
	{
		overrides.pop_back();
		return false;
	}
	return true;
}

bool CSE_InstanceRecord::clear_override(LPCSTR key)
{
	if (is_opaque())
		return false;
	for (auto it = overrides.begin(); it != overrides.end(); ++it)
		if (!xr_strcmp(it->key, key))
		{
			overrides.erase(it);
			return true;
		}
	return false;
}

SInstancePart* CSE_InstanceRecord::find_part(u16 id)
{
	for (SInstancePart& p : parts)
		if (p.id == id)
			return &p;
	return nullptr;
}

const SInstancePart* CSE_InstanceRecord::find_part(u16 id) const
{
	return const_cast<CSE_InstanceRecord*>(this)->find_part(id);
}

u16 CSE_InstanceRecord::add_part(u16 parent, LPCSTR mount, LPCSTR section, float pos)
{
	if (is_opaque() || parts.size() >= MAX_PARTS || next_part_id == u16(-1) || !str_ok(mount) || !_valid(pos))
		return 0;
	if (section && xr_strlen(section) > MAX_STRING)
		return 0;
	if (parent && !find_part(parent))
		return 0;

	SInstancePart part;
	part.id = next_part_id;
	part.parent = parent;
	part.mount = mount;
	part.pos = pos;
	part.section = norm(section);
	parts.push_back(part);
	++next_part_id;
	if (byte_size() > MAX_BYTES)
	{
		parts.pop_back();
		--next_part_id;
		return 0;
	}
	return part.id;
}

u32 CSE_InstanceRecord::remove_part(u16 id)
{
	if (is_opaque() || !find_part(id))
		return 0;

	// Collect the part and everything below it
	xr_vector<u16> doomed;
	doomed.push_back(id);
	for (u32 i = 0; i < doomed.size(); ++i)
		for (const SInstancePart& p : parts)
			if (p.parent == doomed[i])
				doomed.push_back(p.id);

	const u32 before = u32(parts.size());
	parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const SInstancePart& p)
	{
		return std::find(doomed.begin(), doomed.end(), p.id) != doomed.end();
	}), parts.end());
	return before - u32(parts.size());
}

// ---------------------------------------------------------------------------------------------
// Self test (console: instance_record_test)

bool CSE_InstanceRecord::self_test()
{
	u32 failed = 0;
	auto check = [&](bool ok, LPCSTR what)
	{
		Msg("%s [instance record test] %s", ok ? "-" : "!", what);
		if (!ok) ++failed;
	};
	const u32 sentinel = 0xC0FFEE11;
	NET_Packet P;

	// 1. Empty record: one byte, reads back empty
	{
		CSE_InstanceRecord a, b;
		P.write_start();
		a.write(P);
		P.w_u32(sentinel);
		P.r_pos = 0;
		b.set_override("x", "1");
		b.read(P);
		check(P.B.count == 5 && b.empty() && P.r_u32() == sentinel, "empty record is one byte and reads back empty");
	}

	// 2. Full record round trip
	CSE_InstanceRecord full;
	{
		full.set_override("rpm", "650");
		full.set_override("inv_name", "test gun");
		const u16 sight = full.add_part(0, "scope", "mw_sight_pso1", 0.05f);
		const u16 mount = full.add_part(0, "top", "mw_rail_mount");
		const u16 child = full.add_part(mount, "top", "mw_laser_dbal", 0.02f);
		full.add_part(0, "muzzle", "");   // emptied on purpose
		SInstancePart* s = full.find_part(sight);
		s->props.push_back({ "canted", "1" });
		s->upgrades.push_back("up_sight_zero");
		s->set_condition(0.5f);
		for (u8 b : { u8(1), u8(2), u8(3), u8(250) })
			s->blob.push_back(b);
		s->paintjob = "pj_desert";
		full.runtime.valid = true;
		full.runtime.fire_mode = 2;
		full.runtime.gl_mode = 0;
		for (u8 b : { u8(0), u8(0), u8(1), u8(1), u8(0) })
			full.runtime.rounds.push_back(b);

		CSE_InstanceRecord b;
		P.write_start();
		full.write(P);
		const u32 written = P.B.count;
		P.w_u32(sentinel);
		P.r_pos = 0;
		b.read(P);
		check(b == full, "full record reads back equal");
		check(P.r_u32() == sentinel, "full record frame is consumed exactly");
		check(written == full.byte_size(), "byte_size matches the written size");
		check(child != 0 && b.find_part(child) && b.find_part(child)->parent == mount, "nested part keeps its parent");

		// Byte-identical second write
		NET_Packet Q;
		Q.write_start();
		b.write(Q);
		check(Q.B.count == written && !memcmp(Q.B.data, P.B.data, written), "second write is byte identical");
	}

	// 3. Record from a newer format: kept unchanged, read-only
	{
		const u8 body[] = { 7, 7, 7 };
		P.write_start();
		P.w_u8(VERSION + 1);
		P.w_u16(sizeof(body));
		P.w(body, sizeof(body));
		const u32 written = P.B.count;
		P.w_u32(sentinel);
		P.r_pos = 0;
		CSE_InstanceRecord b;
		b.read(P);
		check(b.is_opaque() && P.r_u32() == sentinel, "newer record is kept opaque and its frame consumed");
		check(!b.set_override("rpm", "1") && !b.add_part(0, "scope", "x"), "newer record refuses edits");
		NET_Packet Q;
		Q.write_start();
		b.write(Q);
		check(Q.B.count == written && !memcmp(Q.B.data, P.B.data, written), "newer record writes back unchanged");
	}

	// 4. Damaged body: ignored, frame still consumed
	{
		P.write_start();
		P.w_u8(VERSION);
		P.w_u16(3);
		P.w_u16(500);   // claims 500 overrides
		P.w_u8(0);
		P.w_u32(sentinel);
		P.r_pos = 0;
		CSE_InstanceRecord b;
		b.read(P);
		check(b.empty() && P.r_u32() == sentinel, "damaged record is ignored and its frame consumed");
	}

	// 5. Editing rules
	{
		CSE_InstanceRecord e = full;
		const u16 mount = e.parts[1].id;
		check(e.remove_part(mount) == 2 && e.parts.size() == 2, "removing a part removes its children");
		const u16 id = e.add_part(0, "grip", "mw_grip");
		check(id == full.next_part_id, "part ids are never reused");
		check(e.add_part(999, "x", "y") == 0, "unknown parent is refused");
		CSE_InstanceRecord c;
		u32 n = 0;
		string32 k;
		while (n < MAX_OVERRIDES + 5)
		{
			xr_sprintf(k, "key_%u", n);
			if (!c.set_override(k, "v")) break;
			++n;
		}
		check(n == MAX_OVERRIDES, "override cap is enforced");
		check(c.clear_override("key_0") && !c.get_override("key_0"), "override can be cleared");
	}

	Msg("%s [instance record test] %s", failed ? "!" : "-", failed ? "FAILED" : "all checks passed");
	return failed == 0;
}
