"""Request bodies for the REST API."""

from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, ConfigDict, Field


class PairRequest(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)


class PairConfirm(BaseModel):
    code: str = Field(pattern=r"^\d{4}$")
    name: Optional[str] = Field(default=None, max_length=64)


class RecordingPatch(BaseModel):
    title: str = Field(min_length=1, max_length=200)


class SentencePatch(BaseModel):
    text: Optional[str] = Field(default=None, max_length=5000)
    speaker_idx: Optional[int] = Field(default=None, ge=1, le=99)


class SpeakerPatch(BaseModel):
    name: str = Field(min_length=1, max_length=64)


class SpeakerMerge(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    from_idx: int = Field(alias="from", ge=1, le=99)
    into: int = Field(ge=1, le=99)
