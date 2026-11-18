"""The auth node's model: users and sessions."""


def find_user(conn, username: str) -> dict | None:
    return conn.execute(
        "SELECT id, username, full_name, role, password_hash FROM users WHERE username = %s",
        (username,),
    ).fetchone()


def create_session(conn, user_id: int, lifetime_seconds: int) -> dict:
    return conn.execute(
        """INSERT INTO sessions (user_id, expires_at)
           VALUES (%s, now() + make_interval(secs => %s))
           RETURNING id, issued_at, expires_at""",
        (user_id, lifetime_seconds),
    ).fetchone()


def revoke_session(conn, session_id: str) -> bool:
    """True if the session was open until now, False if it had been signed out before."""
    row = conn.execute(
        "UPDATE sessions SET revoked_at = now() WHERE id = %s AND revoked_at IS NULL RETURNING id",
        (session_id,),
    ).fetchone()
    return row is not None
